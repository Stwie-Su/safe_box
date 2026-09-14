/**
 * @file backend_fm225.c
 * FM22x 双目人脸识别模组后端 —— UART 实现（Sprint3 步骤 3c）。
 *
 * 协议依据《FM22x 系列人脸锁算法模组用户开发手册 V1.7》：
 *   - 帧格式见 fm225_proto.h（EF AA + MsgID + Size(大端) + Data + XOR）；
 *   - H>>M 命令：VERIFY(0x12, pd_rightaway/timeout) / ENROLL(0x13, admin/name/dir/timeout)
 *     / DELETE_USER(0x20, uid heb+leb) / DELETE_ALL(0x21) / FACE_RESET(0x23)；
 *   - M>>H REPLY(0x00)：Data = mid + result(MR_*) + [业务数据]；
 *   - M>>H NOTE(0x01)：Data = nid + [人脸状态等]，NID_READY(0) 上电就绪通知。
 *
 * 数据流（规约 §5.19 接线）：
 *   主线程 face_service_init("fm225") → fm225_open()（SAFE_FM225_DEV，
 *   兼容旧名 SAFE_FACE_DEV，默认 /tmp/fm225_host；上板 /dev/ttymxc2）
 *   + termios 115200 8N1；
 *   face_service_start → fm225_start() 下发 VERIFY 之外还把 fd 经
 *   fm225_uart_fd() 交给 face_thread_set_uart_fd()（start 前调用无竞争）。
 *   face 线程 poll 到 UART 可读 → read → fm225_proto_feed → 帧回调（本文件
 *   on_fm225_frame，仍在 face 线程）→ 识别结果 face_service_emit → event_bus_post。
 *   fd 归本后端所有，close 在 face_service_deinit（app_shutdown 里排在
 *   face_thread_join 之后，线程先停再关 fd，无竞争窗口）。
 *
 * 命令应答超时重发：VERIFY/ENROLL/DELETE 下发后记 pending 命令与时间戳，
 * backend tick（主线程 20ms）查超时（REPLY_TIMEOUT_MS），超时重发，最多
 * FM225_MAX_RETRY 次后报 FACE_EV_ERROR 放弃。
 *
 * 手册警告：模组未上电时若 UART 已连外部设备，外部设备 UART 须为低电平
 * ——真机接线时注意上电顺序；PC 虚拟串口无此约束。
 */

#include "face_backend.h"
#include "fm225_proto.h"
#include "hal/face/face_thread.h"
#include "hal/hal_time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ---------------- 命令参数 ---------------- */

#define FM225_VERIFY_TIMEOUT_S   10     /* verify 超时（手册默认 10s，最大 255） */
#define FM225_ENROLL_TIMEOUT_S   10     /* enroll 超时（手册默认 10s） */
#define FM225_REPLY_TIMEOUT_MS   800    /* 命令应答超时（模组处理远快于 1s） */
#define FM225_MAX_RETRY          3      /* 超时重发上限，超过报 ERROR 放弃 */
#define FM225_VERIFY_REARM_MS    2000   /* verify 会话断链后的重开退避 */

typedef enum {
    FM225_IDLE = 0,
    FM225_WAIT_VERIFY,
    FM225_WAIT_ENROLL,
    FM225_WAIT_DELETE,
} fm225_state_t;

static fm225_state_t s_state = FM225_IDLE;
static bool          s_started;
static int           s_uart_fd = -1;

/* 待应答命令的簿记：重发需要原始帧 */
static uint8_t  s_pending_cmd;             /* H>>M 命令字 */
static uint8_t  s_pending_frame[64];       /* 已下发帧（重发原样） */
static size_t   s_pending_len;
static uint32_t s_pending_at_ms;           /* 下发（或上次重发）时刻 */
static uint8_t  s_retry;                   /* 已重发次数 */
static int32_t  s_delete_id = -1;         /* 待删除 face_id（应答回填） */

static fm225_proto_ctx_t s_proto;

/* ---------------- 帧下发 ---------------- */

static safe_err_t fm225_send_raw(const uint8_t * buf, size_t len)
{
    if(s_uart_fd < 0) return SAFE_ERR_STATE;
    size_t off = 0;
    while(off < len) {
        ssize_t n = write(s_uart_fd, buf + off, len - off);
        if(n < 0) {
            if(errno == EINTR) continue;
            return SAFE_ERR_FAIL;
        }
        off += (size_t)n;
    }
    return SAFE_OK;
}

/* 拼帧并下发：EF AA + cmd + size(大端) + payload + XOR。记录帧副本供重发。 */
static safe_err_t fm225_send_cmd(uint8_t cmd, const uint8_t * payload, size_t len)
{
    if(len > sizeof(s_pending_frame) - 7) return SAFE_ERR_PARAM;

    uint8_t buf[64];
    size_t k = 0;
    buf[k++] = FM225_SYNC0;
    buf[k++] = FM225_SYNC1;
    buf[k++] = cmd;
    buf[k++] = (uint8_t)(len >> 8);
    buf[k++] = (uint8_t)(len & 0xFF);
    uint8_t x = cmd ^ (uint8_t)(len >> 8) ^ (uint8_t)(len & 0xFF);
    for(size_t i = 0; i < len; i++) { buf[k++] = payload[i]; x ^= payload[i]; }
    buf[k++] = x;

    safe_err_t e = fm225_send_raw(buf, k);
    if(e != SAFE_OK) return e;

    s_pending_cmd = cmd;
    memcpy(s_pending_frame, buf, k);
    s_pending_len  = k;
    s_pending_at_ms = hal_time_ms();
    s_retry        = 0;
    return SAFE_OK;
}

/* ---------------- 串口 ---------------- */

static safe_err_t fm225_open(void)
{
    /* 设备节点优先级：SAFE_FM225_DEV（上板预备新增，语义明确）
     *             > SAFE_FACE_DEV（旧名，保持兼容）
     *             > /tmp/fm225_host（PC：socat 虚拟串口）。
     * 上板：SAFE_FM225_DEV=/dev/ttymxc2（i.MX6ULL UART3，115200 8N1）。 */
    const char * dev = getenv("SAFE_FM225_DEV");
    if(dev == NULL || *dev == '\0') dev = getenv("SAFE_FACE_DEV");
    if(dev == NULL || *dev == '\0') dev = "/tmp/fm225_host";

    s_uart_fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(s_uart_fd < 0) {
        printf("[fm225] open %s 失败：%s（PC 联调先起 socat，见 tools/fm225_sim.py 头注释）\n",
               dev, strerror(errno));
        return SAFE_ERR_FAIL;
    }

    /* 115200 8N1 无流控（手册 §五(一)） */
    struct termios tio;
    if(tcgetattr(s_uart_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tcsetattr(s_uart_fd, TCSANOW, &tio);
    }
    /* pty 侧 tcsetattr 可能失败（虚拟串口不支持全部参数）——非致命，字节流照通 */

    printf("[fm225] 已打开 %s（fd=%d，115200 8N1）\n", dev, s_uart_fd);
    return SAFE_OK;
}

static void fm225_close(void)
{
    if(s_uart_fd >= 0) close(s_uart_fd);
    s_uart_fd      = -1;
    s_pending_len  = 0;
}

/* face 线程接入点（§5.19）：start 时把 fd 借给 face 线程 poll。 */
int fm225_uart_fd(void)
{
    return s_uart_fd;
}

/* ---------------- face 线程字节流入口 ---------------- */

/* face_thread.c 在 UART 可读时调用（fd 借用方），把字节喂协议状态机。 */
void fm225_backend_feed(const uint8_t * buf, size_t n)
{
    fm225_proto_feed(&s_proto, buf, n);
}

/* 重发最近一次下发的命令帧（s_pending_* 原样） */
static safe_err_t fm225_resend(void)
{
    if(s_pending_len == 0) return SAFE_ERR_STATE;
    safe_err_t e = fm225_send_raw(s_pending_frame, s_pending_len);
    s_pending_at_ms = hal_time_ms();
    s_retry++;
    return e;
}

/* ---------------- 帧回调（face 线程内执行） ---------------- */
/* MR_* → face_reason_t（规约 §5.20 映射表） */
static face_reason_t mr_to_reason(uint8_t result)
{
    switch(result) {
        case FM225_MR_SUCCESS:           return FACE_RES_OK;
        case FM225_MR_FAILED4_LIVENESS:  return FACE_RES_LIVENESS_FAIL;
        case FM225_MR_FAILED4_TIMEOUT:   return FACE_RES_TIMEOUT;
        case FM225_MR_FAILED4_UNKNOWNUSER: return FACE_RES_NO_MATCH;
        case FM225_MR_ABORTED:            return FACE_RES_NO_MATCH;  /* 验证被终止：同未匹配 */
        default:                          return FACE_RES_ERROR;
    }
}

static void on_fm225_frame(const fm225_frame_t * f, void * user)
{
    (void)user;

    if(f->msgid == FM225_MSGID_NOTE) {
        if(f->mid_or_nid == FM225_NID_READY) {
            /* 模组上电就绪：清 pending（若有旧命令等待，模组重启前的应答不会来了） */
            if(s_state != FM225_IDLE) {
                s_state        = FM225_IDLE;
                s_pending_len = 0;
            }
            printf("[fm225] NOTE READY：模组就绪\n");
        }
        /* NID_FACE_STATE：录入/验证过程中的引导信息，本链路不消费（UI 引导留待后续） */
        return;
    }

    if(f->msgid != FM225_MSGID_REPLY) return;   /* IMAGE 等不解析 */

    /* REPLY 应答受理策略：
     *  - VERIFY 一律受理——模组一个会话只有一条 VERIFY 在飞，迟到/跨重发应答
     *    比丢弃好（模组侧已出结果，丢了就永远丢）；
     *  - ENROLL/DELETE 须对上 pending 命令——防与其它命令应答串话；无 pending
     *    时的迟到应答丢弃（作业早已超时上报）。 */
    if(f->mid_or_nid != FM225_CMD_VERIFY &&
       (s_pending_len == 0 || f->mid_or_nid != s_pending_cmd)) return;
    if(s_pending_len != 0 && f->mid_or_nid == s_pending_cmd) {
        s_pending_len   = 0;                 /* 应答落地，停止重发计时 */
        s_pending_at_ms = hal_time_ms();     /* 会话结束时刻 → 轮间退避基准（D1②） */
    }

    switch(f->mid_or_nid) {
    case FM225_CMD_VERIFY: {
        s_state = FM225_IDLE;
        face_reason_t reason = mr_to_reason(f->result);
        int32_t uid = -1;
        if(reason == FACE_RES_OK && f->data_len >= 2) {
            /* verify 成功携带 user_id_heb/user_id_leb（手册 REPLY mid=VERIFY） */
            uid = (int32_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
        }
        face_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_id   = (reason == FACE_RES_OK) ? uid : -1;
        r.reason    = reason;
        r.timestamp = hal_time();
        printf("[fm225] verify 应答：reason=%s uid=%d\n", face_reason_name(reason), r.face_id);
        face_service_emit(FACE_EV_DETECT, &r);
        /* verify 结束：不在应答回调里立刻续发下一轮——那等于零退避轮询，模组/模拟器
         * 连答时会被打成一串 no_match 洪泛（伪造失败计数 → 自锁）。改由 fm225_tick
         * 的 REARM 退避统一续发，轮间强制留静默间隔（模组一次 VERIFY 只出一组结果）。 */
        break;
    }
    case FM225_CMD_ENROLL: {
        s_state = FM225_IDLE;
        face_enroll_result_t r;
        memset(&r, 0, sizeof(r));
        r.err = (f->result == FM225_MR_SUCCESS) ? SAFE_OK : SAFE_ERR_FAIL;
        if(r.err == SAFE_OK && f->data_len >= 2) {
            r.face_id = (int32_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
        } else {
            r.face_id = -1;
        }
        printf("[fm225] enroll 应答：err=%d uid=%d\n", r.err, r.face_id);
        face_service_emit(FACE_EV_ENROLL_DONE, &r);
        break;
    }
    case FM225_CMD_DELETE_USER:
    case FM225_CMD_DELETE_ALL: {
        int32_t id = s_delete_id;
        s_state     = FM225_IDLE;
        s_delete_id = -1;
        face_delete_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_id = id;
        r.err     = (f->result == FM225_MR_SUCCESS) ? SAFE_OK : SAFE_ERR_FAIL;
        printf("[fm225] delete 应答：err=%d id=%d\n", r.err, r.face_id);
        face_service_emit(FACE_EV_DELETE_DONE, &r);
        break;
    }
    default:
        break;   /* 其余命令的应答（GET_STATUS 等）：不消费 */
    }
}

/* ---------------- 后端接口实现 ---------------- */

static safe_err_t fm225_init(void)
{
    s_state        = FM225_IDLE;
    s_started      = false;
    s_pending_len  = 0;
    s_delete_id    = -1;

    fm225_proto_init(&s_proto, on_fm225_frame, NULL);

    safe_err_t e = fm225_open();
    if(e != SAFE_OK) {
        /* 串口没起（PC 上没先跑 socat）：照常初始化，start 时再报错——
         * face_service_init 的降级逻辑（找不到后端退化 none）不该因外设
         * 临时不在而改变后端选择。 */
        printf("[fm225] 串口未就绪，start 时重试打开\n");
        return SAFE_OK;
    }
    return SAFE_OK;
}

static safe_err_t fm225_deinit(void)
{
    fm225_close();
    return SAFE_OK;
}

static safe_err_t fm225_start(void)
{
    if(s_uart_fd < 0) {
        /* init 时没打开成功（socat 后起）：再试一次，仍失败则报错 */
        safe_err_t e = fm225_open();
        if(e != SAFE_OK) return e;
    }

    /* UART fd 借给 face 线程 poll（§5.19：start 前调用，线程未起无竞争；
     * face_thread_set_uart_fd 幂等保护：线程已在跑则忽略，本后端必须
     * 在 face_thread_start 之前完成 face_service_start——app_main 的
     * face_thread_start() 在 face_service_init/start 之前调用。见 app.c
     * 注释与 fm225_uart_fd 交接顺序） */
    face_thread_set_uart_fd(s_uart_fd);

    s_started = true;

    /* 首轮 VERIFY：pd_rightaway=0（不断电），timeout=手册默认 10s */
    const uint8_t p[] = { 0x00, FM225_VERIFY_TIMEOUT_S };
    safe_err_t e = fm225_send_cmd(FM225_CMD_VERIFY, p, sizeof(p));
    s_state = (e == SAFE_OK) ? FM225_WAIT_VERIFY : FM225_IDLE;
    return e;
}

static safe_err_t fm225_stop(void)
{
    s_started = false;
    /* FACE_RESET：模组终止当前命令回 STANDBY（手册 §MID_RESET） */
    fm225_send_cmd(FM225_CMD_RESET, NULL, 0);
    s_state        = FM225_IDLE;
    s_pending_len  = 0;
    return SAFE_OK;
}

static void fm225_tick(uint32_t now_ms)
{
    /* 协议状态机半帧超时推进（feed 在 face 线程，tick 在主线程，见 §5.20 线程模型） */
    fm225_proto_tick(&s_proto, now_ms);

    /* 命令应答超时重发。now_ms 与 s_pending_at_ms 同为 hal_time_ms() 同源单调毫秒，
     * 差值才是真实等待时长（D1 前：20ms 自增计数 vs 绝对 ms 跨基 → 回绕后恒真）。 */
    if(s_pending_len > 0 && s_started &&
       (now_ms - s_pending_at_ms) > FM225_REPLY_TIMEOUT_MS) {
        if(s_retry >= FM225_MAX_RETRY) {
            s_pending_len = 0;
            s_state = FM225_IDLE;
            char msg[64];
            snprintf(msg, sizeof(msg), "fm225 命令 0x%02X 无应答（重发 %u 次）",
                     s_pending_cmd, s_retry);
            printf("[fm225] %s\n", msg);
            face_service_emit(FACE_EV_ERROR, msg);
        }
        else {
            /* 日志降频（D1③）：仅在「进入重发」这一状态变化时打一次；中间几次重发
             * 静默，避免刷屏淹没真实事件。放弃时另打一条（见上，无应答）。 */
            if(s_retry == 0) {
                printf("[fm225] 命令 0x%02X 无应答，开始重发（上限 %u 次）\n",
                       s_pending_cmd, FM225_MAX_RETRY);
            }
            fm225_resend();
        }
    }

    /* verify 会话续发（退避，D1②）：运行中但无 pending 命令——可能是重发放弃、
     * 应答已处理、或模组重启。等 FM225_VERIFY_REARM_MS 后重开本轮：既保活识别链路
     * （不能一次失败就停摆），又保证轮间静默、不刷屏。s_pending_at_ms 在无 pending
     * 时 =「上次命令下发 / 应答落地」时刻，即退避基准。 */
    if(s_started && s_pending_len == 0 && s_state != FM225_WAIT_ENROLL &&
       (now_ms - s_pending_at_ms) > FM225_VERIFY_REARM_MS) {
        const uint8_t p[] = { 0x00, FM225_VERIFY_TIMEOUT_S };
        if(fm225_send_cmd(FM225_CMD_VERIFY, p, sizeof(p)) == SAFE_OK)
            s_state = FM225_WAIT_VERIFY;
        /* send 失败（串口坏）：下个 tick 再试，不刷屏 */
    }
}

static safe_err_t fm225_enroll(void)
{
    if(s_state == FM225_WAIT_ENROLL) return SAFE_ERR_BUSY;
    /* ENROLL：admin=0（业务层另行记角色，模组 admin 标记不用），姓名 32B 全 0，
     * 方向 UNDEFINE（模组按默认正向流程交互录入），timeout=10s */
    uint8_t p[35];
    memset(p, 0, sizeof(p));
    p[34] = FM225_ENROLL_TIMEOUT_S;
    safe_err_t e = fm225_send_cmd(FM225_CMD_ENROLL, p, sizeof(p));
    s_state = (e == SAFE_OK) ? FM225_WAIT_ENROLL : FM225_IDLE;
    return e;
}

static safe_err_t fm225_delete_tpl(int32_t face_id)
{
    if(face_id < 0) return SAFE_ERR_PARAM;
    if(s_state == FM225_WAIT_DELETE) return SAFE_ERR_BUSY;
    uint8_t p[2] = {
        (uint8_t)((uint16_t)face_id >> 8),   /* user_id_heb */
        (uint8_t)(face_id & 0xFF),           /* user_id_leb */
    };
    safe_err_t e = fm225_send_cmd(FM225_CMD_DELETE_USER, p, sizeof(p));
    s_state    = (e == SAFE_OK) ? FM225_WAIT_DELETE : FM225_IDLE;
    s_delete_id = (e == SAFE_OK) ? face_id : -1;
    return e;
}

/* 真实模组不支持注入，调用方应在调用前查 FACE_CAP_INJECT */
static safe_err_t fm225_inject(int32_t face_id, face_reason_t reason)
{
    (void)face_id; (void)reason;
    return SAFE_ERR_UNSUP;
}

static const face_backend_t backend = {
    .name          = "fm225",
    .caps          = FACE_CAP_DETECT | FACE_CAP_ENROLL | FACE_CAP_DELETE | FACE_CAP_LIVENESS,
    .max_templates = 1000,
    .init          = fm225_init,
    .deinit        = fm225_deinit,
    .start         = fm225_start,
    .stop          = fm225_stop,
    .tick          = fm225_tick,
    .enroll        = fm225_enroll,
    .delete_tpl    = fm225_delete_tpl,
    .inject        = fm225_inject,
};

const face_backend_t * face_backend_fm225(void)
{
    return &backend;
}

