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
 * backend tick（主线程 20ms）查超时——即时命令用 REPLY_TIMEOUT_MS(800ms)，
 * 会话命令 VERIFY/ENROLL 用 SESSION 超时（模组 timeout 参数+余量，D17），
 * 超时重发，最多 FM225_MAX_RETRY 次后报 FACE_EV_ERROR 放弃。
 *
 * 手册警告：模组未上电时若 UART 已连外部设备，外部设备 UART 须为低电平
 * ——真机接线时注意上电顺序；PC 虚拟串口无此约束。
 */

#include "face_backend.h"
#include "fm225_proto.h"
#include "hal/hal_face.h"
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
#define FM225_REPLY_TIMEOUT_MS   800    /* 即时命令（RESET/GETSTATUS/DELETE）应答超时 */
/* 会话型命令（VERIFY/ENROLL）的应答超时：模组要等人脸/录入完成才回最终 REPLY，
 * 过程中只回 NOTE（手册 §MID_VERIFY「解锁过程中，模组返回 NOTE 和 REPLY 两种
 * 消息」）。pending 超时必须 ≥ 模组超时 + 余量——D17：此前沿用 800ms 即时超时，
 * sim 秒回应答掩盖了该语义差异，真模组在 800ms 判定下永远「无应答」。 */
#define FM225_SESSION_TIMEOUT_MS  ((FM225_VERIFY_TIMEOUT_S + 2u) * 1000u)
#define FM225_MAX_RETRY          3      /* 超时重发上限，超过报 ERROR 放弃 */
#define FM225_VERIFY_REARM_MS    2000   /* verify 会话断链后的重开退避 */
#define FM225_RECON_RETRY_MS     1500   /* 0x24 清单查询的重试间隔（静默查询，无 ERROR 上报） */
#define FM225_RECON_MAX_IDS      128    /* 本地缓存上限（实测模组容量 100） */
#define FM225_HEALTH_ROUND_MS    5000u  /* 健康评估轮长：每轮看是否有合法帧（FR-23） */
#define FM225_HEALTH_FAIL_ROUNDS 3u     /* 连续 3 轮无应答 → 不健康（≈15s） */

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

/* 模组侧已注册用户清单（0x24 应答缓存，FR-21 防线 3 对账用）。
 * s_mod_id_count: -1 = 尚未取得；>=0 = 已取得（0 是合法值，必须与「没取到」区分）。 */
static int32_t  s_mod_ids[FM225_RECON_MAX_IDS];
static int32_t  s_mod_id_count = -1;
static uint32_t s_recon_at_ms;             /* 上次发出清单查询的时刻（节流用） */

/* 模组健康监测（FR-23「模组健康与降级」）。
 * s_health 只由主线程（fm225_tick / fm225_init / fm225_deinit）写、fm225_module_health 读；
 * s_rx_in_round 由 face 线程（on_fm225_frame）置位、主线程读取并清零——跨线程单标量，
 * 最坏情况漏/迟一拍，对「本轮是否收到过帧」的判定无实质影响（与 s_mod_id_count 同风格）。 */
static face_module_health_t s_health = FACE_MOD_UNKNOWN;
static bool     s_health_monitor;      /* 监测开关：init 开、deinit 关 */
static uint32_t s_health_round_at_ms;  /* 本轮起点（0 = 尚未开始计时） */
static uint32_t s_health_miss_rounds;  /* 连续无应答轮次 */
static bool     s_rx_in_round;         /* 本轮是否收到过合法帧 */

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

/* 下发一条「不问结果」的即时查询（对账用）。
 * 不走 fm225_send_cmd：那条路径会记 pending 并在超时后重发 + 报 FACE_EV_ERROR，
 * 而清单查询失败属「尽力而为」，不该在界面弹错误横幅、更不该刷屏重发。 */
static void fm225_send_silent(uint8_t cmd)
{
    const uint8_t frame[6] = { FM225_SYNC0, FM225_SYNC1, cmd, 0x00, 0x00, cmd };
    fm225_send_raw(frame, sizeof(frame));   /* XOR = cmd ^ 0 ^ 0 = cmd */
}

/* 启动对账第一步：取模组用户清单（FR-21 防线 3）。
 * 只在「空闲且无 pending」时发 —— 实测（2026-09-15）新命令会**抢占**进行中的会话，
 * 且被抢占的会话静默无应答；识别链路优先，查询让路。 */
static void fm225_recon_poll(uint32_t now_ms)
{
    if(s_mod_id_count >= 0) return;                     /* 已取得，不再查 */
    if(!s_started || s_pending_len > 0 || s_state != FM225_IDLE) return;
    if(s_recon_at_ms != 0 && (now_ms - s_recon_at_ms) < FM225_RECON_RETRY_MS) return;
    s_recon_at_ms = now_ms;
    fm225_send_silent(FM225_CMD_GET_ALL_USERID);
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

/* ---------------- 模组健康监测（FR-23） ---------------- */

/* 收到任何合法帧：标记本轮有应答（FR-23 恢复判据）。在 face 线程内调用
 * （on_fm225_frame 首行）。只置标志，状态翻转统一由主线程 fm225_health_tick 判定，
 * 避免 face 线程与主线程同时写 s_health。 */
static void fm225_health_on_rx(void)
{
    s_rx_in_round = true;
}

/* 状态翻转时打**一条**日志（FR-23 防刷屏）：仅在状态真正变化时打印。
 * UNKNOWN→OK 静默（启动首次出帧不算故障翻转）；UNKNOWN→FAIL 打印（从未知进入不健康）。 */
static void fm225_set_health(face_module_health_t st)
{
    if(st == s_health) return;
    face_module_health_t old = s_health;
    s_health = st;
    if(st == FACE_MOD_FAIL) {
        printf("[fm225] 模组健康：%s -> 不健康（连续 %u 轮无应答，约 %us）\n",
               old == FACE_MOD_UNKNOWN ? "未知" : "健康",
               (unsigned)s_health_miss_rounds,
               (unsigned)(FM225_HEALTH_ROUND_MS / 1000u * FM225_HEALTH_FAIL_ROUNDS));
    }
    else if(st == FACE_MOD_OK && old == FACE_MOD_FAIL) {
        printf("[fm225] 模组健康：不健康 -> 健康（收到合法帧，恢复）\n");
    }
}

/* Duty-cycle（FR-27）：前台标志边沿处理（主线程 tick 驱动）。离开时作废 pending 并用
 * 0x10 RESET 终止进行中的会话（实测 26ms 回执、能立即终止；被终止方静默无应答，
 * 故 pending 直接作废不等 REPLY）；进入时不主动发包，会话由既有 REARM 退避自然重开。 */
static bool s_fg_last = true;      /* 上一次 tick 的前台态（初值 = 前台，与 face_service 一致） */
static void fm225_fg_tick(uint32_t now_ms)
{
    (void)now_ms;
    bool fg = face_service_foreground();
    if(fg == s_fg_last) return;
    s_fg_last = fg;
    if(!fg) {
        if(s_pending_len > 0 || s_state != FM225_IDLE) {
            fm225_send_silent(FM225_CMD_RESET);
            printf("[fm225] 离开人脸页：0x10 RESET 终止会话\n");
        }
        s_pending_len = 0;
        s_state       = FM225_IDLE;
    }
    else {
        printf("[fm225] 进入人脸页：恢复识别会话\n");
    }
}

/* 健康监测（主线程 20ms tick 驱动，FR-23，不新增线程）：以 5s 为一轮，连续 3 轮（≈15s）
 * 内没有任何合法帧 → 不健康；本轮内收到过帧即清零连无应答计数并立即恢复。 */
static void fm225_health_tick(uint32_t now_ms)
{
    if(!s_health_monitor) return;
    /* Duty-cycle（FR-27）：前台暂停期模组是被我们主动静默的，不计无应答轮；
     * 清轮基准，恢复前台后从零重新起算（恢复后仍要连续 3 轮静默才判不健康）。 */
    if(!face_service_foreground()) { s_health_round_at_ms = 0; return; }
    if(s_health_round_at_ms == 0) { s_health_round_at_ms = now_ms; return; }  /* 首轮基准 */

    if(s_rx_in_round) {
        s_rx_in_round        = false;      /* 本轮有应答：清零连无应答计数 */
        s_health_miss_rounds = 0;
        s_health_round_at_ms = now_ms;     /* 以最近一次应答重启轮计时 */
        fm225_set_health(FACE_MOD_OK);     /* 不健康 -> 健康 立即恢复 */
        return;
    }

    if((now_ms - s_health_round_at_ms) < FM225_HEALTH_ROUND_MS) return;   /* 本轮未结束 */

    s_health_round_at_ms = now_ms;         /* 一轮无应答结束 */
    if(s_health_miss_rounds < FM225_HEALTH_FAIL_ROUNDS) s_health_miss_rounds++;
    if(s_health_miss_rounds >= FM225_HEALTH_FAIL_ROUNDS) fm225_set_health(FACE_MOD_FAIL);
}

/* ---------------- 帧回调（face 线程内执行） ---------------- */

/* 把 UTF-8 名字拷进定长字段，不切断多字节字符（放不下的整字丢弃，末尾补 NUL）。
 * 模组侧 user_name 字段是 32B 定长，中文 3B/字 → 最多 10 字（30B），实际够用。 */
static void copy_name_utf8(char * dst, size_t cap, const char * src)
{
    if(dst == NULL || cap == 0) return;
    dst[0] = '\0';
    if(src == NULL) return;
    size_t n = 0;
    while(src[n] != '\0' && n + 1 < cap) {
        unsigned char c = (unsigned char)src[n];
        size_t w = (c < 0x80u) ? 1u : ((c >= 0xF0u) ? 4u : ((c >= 0xE0u) ? 3u : 2u));
        if(n + w > cap - 1) break;          /* 放不下则整字丢弃，不切半个字 */
        n += w;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* MR_* → safe_err_t（v1.4 / N3）：不再把失败原因坍缩成统一的 SAFE_ERR_FAIL。
 * 手册的 MR 码各有语义，坍缩后 UI 只能报「失败」，FR-22「同一张脸只能绑一个用户」
 * 也无从判定。映射遵循工程既有的「错误码统一」原则（§1.2）。 */
static safe_err_t mr_to_err(uint8_t mr)
{
    switch(mr) {
        case FM225_MR_SUCCESS:                return SAFE_OK;
        case FM225_MR_FAILED4_INVALIDPARAM:   return SAFE_ERR_PARAM;
        case FM225_MR_FAILED4_NOMEMORY:       return SAFE_ERR_NOMEM;
        case FM225_MR_FAILED4_MAXUSER:        return SAFE_ERR_NOMEM;   /* 模板容量已满 */
        case FM225_MR_FAILED4_UNKNOWNUSER:    return SAFE_ERR_NOENT;
        case FM225_MR_FAILED4_FACEENROLLED:   return SAFE_ERR_EXIST;   /* 该脸已录入（FR-22） */
        case FM225_MR_FAILED4_TIMEOUT:        return SAFE_ERR_TIMEOUT;
        case FM225_MR_REJECTED:               return SAFE_ERR_STATE;   /* 被拒 / 时序不允许 */
        case FM225_MR_ABORTED:                return SAFE_ERR_STATE;
        default:                              return SAFE_ERR_FAIL;
    }
}

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

    fm225_health_on_rx();   /* FR-23：收到任何合法帧即为「有应答」（恢复判据） */

    if(f->msgid == FM225_MSGID_NOTE) {
        if(f->mid_or_nid == FM225_NID_READY) {
            /* 模组上电就绪：清 pending（若有旧命令等待，模组重启前的应答不会来了） */
            if(s_state != FM225_IDLE) {
                s_state        = FM225_IDLE;
                s_pending_len = 0;
            }
            /* 模组可能刚重启（断电期间模板被人改过是可能的）→ 清单作废、重新取一次 */
            s_mod_id_count = -1;
            s_recon_at_ms  = 0;
            printf("[fm225] NOTE READY：模组就绪\n");
        }
        /* NID_FACE_STATE：录入/验证过程中的人脸状态引导流。实测（2026-09-15 真模组）帧长
         * 17B = nid(1) + s_note_data_face{state,left,top,right,bottom,yaw,pitch,roll}
         * （各 int16，手册 P24），约 2.1 条/秒；无人脸时 state = 1（FACE_STATE_NOFACE）。
         * 这就是 FR-19「多方向交互录入的实时引导」现成的数据源（「请正对镜头」「距离太远」
         * 不必再去问模组别的接口）。当前 UI 未消费 → 本链路暂不解析，但**不把它当垃圾帧**：
         * 它同时是「模组在活跃工作」的证据，将来 FR-23 健康监测可复用。 */
        return;
    }

    if(f->msgid != FM225_MSGID_REPLY) return;   /* IMAGE 等不解析 */

    /* REPLY 应答受理策略：
     *  - VERIFY 一律受理——模组一个会话只有一条 VERIFY 在飞，迟到/跨重发应答
     *    比丢弃好（模组侧已出结果，丢了就永远丢）；
     *  - GET_ALL_USERID 一律受理——它是「静默查询」（fm225_send_silent 不记 pending），
     *    若按 pending 过滤会被整条丢掉，对账永远拿不到清单；
     *  - ENROLL/DELETE 须对上 pending 命令——防与其它命令应答串话；无 pending
     *    时的迟到应答丢弃（作业早已超时上报）。 */
    if(f->mid_or_nid != FM225_CMD_VERIFY &&
       f->mid_or_nid != FM225_CMD_GET_ALL_USERID &&
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
        face_result_t r;
        memset(&r, 0, sizeof(r));
        if(reason == FACE_RES_OK && f->data_len >= 2) {
            /* verify 成功应答 = s_msg_reply_verify_data（手册 P34）：
             * user_id_heb(1) + user_id_leb(1) + user_name[32] + admin(1) + unlockStatus(1)。
             * data 已剥掉 mid/result 前缀，故用户区自 data[0] 起。 */
            uid = (int32_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
            if(f->data_len >= 34) {
                /* v1.4（FR-21 防线 1）：把模组侧模板名带回去，供业务层核对该脸归属。
                 * 字段 32B 定长且不保证 NUL 结尾 —— 拷满后强制补尾 0（极端情况下丢第 32
                 * 字节；中文名 10 字 = 30B，实际碰不到）。 */
                memcpy(r.user_name, f->data + 2, sizeof(r.user_name));
                r.user_name[sizeof(r.user_name) - 1] = '\0';
            }
        }
        r.face_id   = (reason == FACE_RES_OK) ? uid : -1;
        r.reason    = reason;
        r.timestamp = hal_time();
        printf("[fm225] verify 应答：reason=%s uid=%d name=\"%s\"\n",
               face_reason_name(reason), r.face_id, r.user_name);
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
        r.err = mr_to_err(f->result);
        if(r.err == SAFE_OK && f->data_len >= 2) {
            r.face_id = (int32_t)(((uint16_t)f->data[0] << 8) | f->data[1]);
        } else {
            r.face_id = -1;
        }
        /* 同时打原始 MR 码：错误码是给 UI 的语义，MR 码是给排错用的现场。
         * 注意：本文件在 hal 层，不得调 core 的 safe_err_str（hal→core 是反向依赖，
         * 链接期会直接 undefined reference）——只打数值，语义由 UI 层翻译。 */
        printf("[fm225] enroll 应答：err=%d mr=0x%02X uid=%d\n",
               (int)r.err, f->result, r.face_id);
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
        r.err     = mr_to_err(f->result);
        printf("[fm225] delete 应答：err=%d mr=0x%02X id=%d\n",
               (int)r.err, f->result, r.face_id);
        face_service_emit(FACE_EV_DELETE_DONE, &r);
        break;
    }
    case FM225_CMD_GET_ALL_USERID: {
        /* 应答 = mid(1) + result(1) + user_counts(1) + users_id[N*2]（每个 ID 先存高八位）
         * 手册 §MID_GET_ALL_USERID；实测（2026-09-15）data 203B、容量 100。
         * 解析委托给纯函数 fm225_parse_userid_list()：它吃的正是「已剥掉 mid/result
         * 前缀后的 Data 区」（f->data / f->data_len），data[0] 即 user_counts。
         * ⚠️ 历史缺陷（QA 复核 高危#1）：本分支曾直接读 data[2]/data[3..]，等于把
         * user_counts 当成 data[2]、整体错位 2 字节 —— 空模块（全 0）恰好掩盖了它，
         * 真机非空清单会漏掉用户 / 把 uid 读成垃圾。抽函数 + 单测固化防复发。 */
        s_state = FM225_IDLE;
        int32_t cnt = 0;
        if(f->result == FM225_MR_SUCCESS) {
            cnt = (int32_t)fm225_parse_userid_list(f->data, f->data_len,
                                                   s_mod_ids, FM225_RECON_MAX_IDS);
        }
        s_mod_id_count = cnt;
        printf("[fm225] 模组用户清单：%d 个（应答 data %u 字节）\n",
               (int)s_mod_id_count, (unsigned)f->data_len);
        for(int32_t i = 0; i < cnt && i < 8; i++)
            printf("         uid[%d] = %d\n", (int)i, (int)s_mod_ids[i]);
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
    s_mod_id_count = -1;      /* 清单未取得 */
    s_recon_at_ms  = 0;

    /* 健康监测复位并开启（FR-23）：串口没起（PC 无设备）时也要能判「模组无响应」，
     * 故监测只跟后端是否在用有关，与串口是否打开无关。 */
    s_health             = FACE_MOD_UNKNOWN;
    s_health_monitor     = true;
    s_health_round_at_ms = 0;
    s_health_miss_rounds = 0;
    s_rx_in_round        = false;

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
    s_health_monitor = false;                 /* 停止健康监测（FR-23） */
    s_health         = FACE_MOD_UNKNOWN;
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
    /* MID_RESET(0x10)：通用复位。实测（2026-09-15 真模组）：下发后约 26ms 回
     * REPLY(result=0x00)，且进行中的 VERIFY 会话立即停止出 NOTE（静默终止，不回 ABORTED）
     * —— 0x10 足以让会话停下来。
     * 术语订正：本行原注释写作「FACE_RESET」，但手册里 0x10 是 MID_RESET（通用复位），
     * MID_FACERESET 才是 0x23（终止「录入」并清录入状态），两者不是一回事。 */
    fm225_send_cmd(FM225_CMD_RESET, NULL, 0);
    s_state        = FM225_IDLE;
    s_pending_len  = 0;
    return SAFE_OK;
}

static void fm225_tick(uint32_t now_ms)
{
    /* 协议状态机半帧超时推进（feed 在 face 线程，tick 在主线程，见 §5.20 线程模型） */
    fm225_proto_tick(&s_proto, now_ms);

    /* Duty-cycle 边沿（FR-27）：离开人脸页终止会话、进入时恢复 */
    fm225_fg_tick(now_ms);

    /* 模组健康监测（FR-23）：复用主线程 20ms tick，不新增线程（内部有前台守卫） */
    fm225_health_tick(now_ms);

    /* 前台暂停：不对账 / 不重发 / 不重开识别会话 */
    if(!face_service_foreground()) return;

    /* 启动对账：空闲时取一次模组用户清单（FR-21 防线 3） */
    fm225_recon_poll(now_ms);

    /* 命令应答超时重发。now_ms 与 s_pending_at_ms 同为 hal_time_ms() 同源单调毫秒，
     * 差值才是真实等待时长（D1 前：20ms 自增计数 vs 绝对 ms 跨基 → 回绕后恒真）。
     * 超时值按命令分型（D17）：VERIFY/ENROLL 是会话命令用 SESSION 超时，
     * 其余（RESET/GETSTATUS/DELETE）立即应答用 800ms。 */
    uint32_t pending_timeout_ms = FM225_REPLY_TIMEOUT_MS;
    if(s_pending_cmd == FM225_CMD_VERIFY || s_pending_cmd == FM225_CMD_ENROLL)
        pending_timeout_ms = FM225_SESSION_TIMEOUT_MS;
    if(s_pending_len > 0 && s_started &&
       (now_ms - s_pending_at_ms) > pending_timeout_ms) {
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

static safe_err_t fm225_enroll(const char * user_name)
{
    if(s_state == FM225_WAIT_ENROLL) return SAFE_ERR_BUSY;

    /* ENROLL 载荷（手册 §六 H>>M，共 35B）：
     *   p[0]     admin     = 0   角色由业务层记录，模组 admin 标记不用（实测 admin=0 被接受）
     *   p[1..32] user_name 本地用户名 —— v1.4（FR-21 防线 1）：把名字写进模组侧模板，
     *                      VERIFY 成功应答会原样带回，供业务层核对凭据归属
     *   p[33]    face_dir  = 0x00（UNDEFINE，走默认正向交互录入。实测 2026-09-15：
     *                      0x00 与 0x01(FACE_DIRECTION_MIDDLE) 行为完全一致，都进入录入流程
     *                      并在 timeout 后回 MR_FAILED4_TIMEOUT，故维持 0x00）
     *                      ★ 当前拍板（用户 2026-09-15）：**单帧模式**（一次采集即成模板，
     *                      最简单可靠）；多角度方向录入为后续升级项，勿在本函数顺手加）
     *   p[34]    timeout   = FM225_ENROLL_TIMEOUT_S
     *
     * ★ 会话互斥实测（2026-09-15 真模组，勿据直觉改）：VERIFY 会话进行中下发 ENROLL，
     *   模组**接受**新命令并抢占会话 —— ENROLL 自己走满 10s 超时后正常回 REPLY；
     *   而被抢占的 VERIFY **静默终止、不回 REPLY（也没有 ABORTED）**。
     *   所以「每个 VERIFY 必有应答」是错的假设；当前实现自洽靠的是 fm225_send_cmd 会把
     *   s_pending_* 覆盖成新命令（VERIFY 的应答本就不会来）。若将来改成多 pending 并行，
     *   必须同时处理「被抢占的会话永不应答」这件事。 */
    uint8_t p[35];
    memset(p, 0, sizeof(p));
    copy_name_utf8((char *)(p + 1), 32, user_name);
    p[33] = 0x00;
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
/* 模组侧已注册用户清单（FR-21 防线 3）。语义见 hal_face.h。 */
static int32_t fm225_module_users(int32_t * ids, int32_t cap)
{
    if(s_mod_id_count < 0) return -2;                 /* 有能力，但尚未取得 */
    if(ids != NULL && cap > 0) {
        int32_t n = (s_mod_id_count < cap) ? s_mod_id_count : cap;
        for(int32_t i = 0; i < n; i++) ids[i] = s_mod_ids[i];
    }
    return s_mod_id_count;
}

/* 模组健康只读三态（FR-23）。语义见 hal_face.h。 */
static face_module_health_t fm225_module_health(void)
{
    return s_health;
}

static safe_err_t fm225_inject(int32_t face_id, face_reason_t reason)
{
    (void)face_id; (void)reason;
    return SAFE_ERR_UNSUP;
}

static const face_backend_t backend = {
    .name          = "fm225",
    .caps          = FACE_CAP_DETECT | FACE_CAP_ENROLL | FACE_CAP_DELETE | FACE_CAP_LIVENESS,
    /* 容量上限（v1.4 修正，原为硬编码 1000）：实测 0x24 应答 payload = 201B
     * = user_counts(1) + 100×2 → 模组实际可容纳 100 个用户；手册命令表写
     * MAX_USER_COUNTS=50，与实测不符 —— 以实测为准（若固件升级需复测）。
     * 原来的 1000 无任何依据，会让界面长期显示「n/1000」这种假精确。 */
    .max_templates = 100,
    .init          = fm225_init,
    .deinit        = fm225_deinit,
    .start         = fm225_start,
    .stop          = fm225_stop,
    .tick          = fm225_tick,
    .enroll        = fm225_enroll,
    .delete_tpl    = fm225_delete_tpl,
    .module_users  = fm225_module_users,
    .module_health = fm225_module_health,
    .inject        = fm225_inject,
};

const face_backend_t * face_backend_fm225(void)
{
    return &backend;
}

