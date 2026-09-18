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
#define FM225_ENROLL_TIMEOUT_5WAY 30  /* 五向录入超时：五个朝向依次采集，给足冗余 */
#define FM225_REPLY_TIMEOUT_MS   800    /* 即时命令（RESET/GETSTATUS/DELETE）应答超时 */
/* 会话型命令（VERIFY/ENROLL）的应答超时：模组要等人脸/录入完成才回最终 REPLY，
 * 过程中只回 NOTE（手册 §MID_VERIFY「解锁过程中，模组返回 NOTE 和 REPLY 两种
 * 消息」）。pending 超时必须 ≥ 模组超时 + 余量——D17：此前沿用 800ms 即时超时，
 * sim 秒回应答掩盖了该语义差异，真模组在 800ms 判定下永远「无应答」。
 * D18 补：取 VERIFY / ENROLL 两者中较大的模组超时做基准，避免以后单独调大
 * 某一个（比如把录入放宽到 20s）时，会话超时仍按 verify 的 10s 算而误判超时。 */
#define FM225_SESSION_CMD_TIMEOUT_S \
    ((FM225_VERIFY_TIMEOUT_S > FM225_ENROLL_TIMEOUT_S) ? \
     (FM225_VERIFY_TIMEOUT_S) : (FM225_ENROLL_TIMEOUT_S))
#define FM225_SESSION_TIMEOUT_MS  ((FM225_SESSION_CMD_TIMEOUT_S + 2u) * 1000u)
/* 下发会话型命令（ENROLL）前强制留出的静默窗口（ms）。
 * 两个用途：
 *  ① 给模组消化「上一条命令」的时间 —— 0x10 MID_RESET 实测约 26ms 回执，
 *     0x24 清单应答 203B 在 115200（1B≈87us）下约 18ms，300ms 有 10x 余量；
 *  ② 静默命令（fm225_send_silent：不进 pending、不重发）没有超时出口，用同一
 *     窗口兜底判定「它已经不会再来了」，避免模组永不应答 0x24 时永久阻塞 ENROLL。
 * 代价：最坏情况录入多等 300ms 才真正下发（用户无感）。 */
#define FM225_SETTLE_MS          300u
/* 录入会话进行到这个时长、模组还没上报过一条 NOTE 人脸状态时，打一条提示日志
 * （FR-19 可观测性）：区分「模组没进录入」与「人没站到镜头前」。 */
#define FM225_ENROLL_HINT_MS     3000u
#define FM225_MAX_RETRY          3      /* 即时命令超时重发上限；会话型命令不重发（见 fm225_tick） */
#define FM225_VERIFY_REARM_MS    2000   /* verify 会话断链后的重开退避 */
#define FM225_RECON_RETRY_MS     1500   /* 0x24 清单查询的重试间隔（静默查询，无 ERROR 上报） */
#define FM225_RECON_MAX_IDS      128    /* 本地缓存上限（实测模组容量 100） */
#define FM225_HEALTH_ROUND_MS    5000u  /* 健康评估轮长：每轮看是否有合法帧（FR-23） */
#define FM225_HEALTH_FAIL_ROUNDS 3u     /* 连续 3 轮无应答 → 不健康（≈15s） */

/* 0x10 与 0x23 的分工（手册订正，勿再混用）
 *   0x10 MID_RESET（手册 §3）：「模组将取消之前正在执行的命令（例如录入、解锁等），
 *        返回 STANDBY 状态」——通用取消，覆盖面包含录入与解锁。
 *   0x23 MID_FACERESET / FACE RESET（手册 H>>M 命令表 + §录入流程）：「录入过程中
 *        可通过 FACE RESET 指令终止录入，先前的录入状态也会清零」——只管录入状态。
 * 本文件的用法：
 *   - 需要「把模组拉回 STANDBY / 取消任何在途会话」→ 0x10（stop、离页、录入前清场、
 *     会话超时放弃）。只有它能取消在飞的 VERIFY。
 *   - 需要「清掉录入残留状态」→ 0x23（录入以失败/超时收尾后）。 */
typedef enum {
    FM225_IDLE = 0,
    FM225_WAIT_VERIFY,
    FM225_ENROLL_ARMING,   /* 录入请求已受理、尚未下发：等模组空闲 + 静默窗口 */
    FM225_WAIT_ENROLL,     /* ENROLL 已下发：模组录入会话进行中（独占，禁插其它命令） */
    FM225_WAIT_DELETE,
} fm225_state_t;

static fm225_state_t s_state = FM225_IDLE;
static bool          s_started;
static int           s_uart_fd = -1;

/* 待应答命令的簿记：重发需要原始帧 */
static uint8_t  s_pending_cmd;             /* H>>M 命令字 */

/* 模组实时人脸状态（NOTE NID_FACE_STATE 的 state 字段，face 线程写 / 主线程读）。
 * 语义（手册 §NOTE）：0=正常 1=未检测到 2=太靠上 3=太靠下 4=太靠左 5=太靠右；
 * -1 = 尚未收到任何状态帧。volatile 标量，与 s_fg 同一跨线程纪律。 */
static volatile int32_t s_face_state = -1;
static uint8_t  s_pending_frame[64];       /* 已下发帧（重发原样） */
static size_t   s_pending_len;
static uint32_t s_pending_at_ms;           /* 下发（或上次重发）时刻 */
static uint8_t  s_retry;                   /* 已重发次数 */
static int32_t  s_delete_id = -1;         /* 待删除 face_id（应答回填） */

/* 录入请求（受理与下发分离，见 fm225_enroll / fm225_enroll_issue）：
 * 点按钮只做「受理」，真正下发 ENROLL 由 tick 在「模组空闲 + 静默窗口已过」后完成。 */
static bool     s_enroll_req;                          /* 有录入请求待下发 */
static char     s_enroll_name[32];                     /* 待录入的名字（UTF-8，NUL 结尾） */
static uint32_t s_enroll_arm_at_ms;                    /* 受理时刻（静默窗口基准） */
static uint32_t s_enroll_sent_at_ms;                   /* ENROLL 实际下发时刻 */
static uint32_t s_enroll_note_count;                   /* 本次录入会话收到的 NOTE 人脸状态条数 */
static int32_t  s_enroll_last_state;                   /* 会话期间末次人脸状态（-1 = 从未上报） */
static bool     s_enroll_hint_done;                    /* 「模组没上报过状态」提示是否已打过 */

/* 录入模式（可切换，默认单帧）。
 *   单帧：face_direction = FACE_DIRECTION_UNDEFINE(0x00)，一次采集即成模板
 *         （用户 2026-09-15 拍板，最简单可靠）。
 *   五向：face_direction = 0x1F（上|下|左|右|正），一次 ENROLL 命令内由模组
 *         依次采集五个朝向 —— **不是**串联发 5 次命令（手册 V1.7 1062-1071、1083）。
 * 切换方式：env SAFE_FACE_ENROLL_5WAY=1（白名单式，与既有 env 开关同一纪律）。
 * 之所以做成可切换：五向的真机表现尚未验证（此前录入一直超时，face_direction
 * 的多向语义一次都没跑通过），保留单帧做对照，出问题可一键回退。 */
static bool     s_enroll_5way;

/* NOTE 姿态快照（yaw/pitch/roll），与 s_face_state 同一跨线程纪律。
 * 五向引导需要它来判断用户有没有真的转头/抬头。
 * ★ 整组更新：必须与 state 同时刷新，否则 UI 会读到「新 state + 旧姿态」的组合态。 */
static volatile int16_t s_pose_yaw;
static volatile int16_t s_pose_pitch;
static volatile int16_t s_pose_roll;

/* ---------- NOTE 明细诊断（env SAFE_FM225_NOTE_DEBUG，默认关） ----------
 * 用途：定位「模组进了录入会话却看不到脸」这类问题——需要看模组**到底上报了什么**：
 * state 数值、人脸框 left/top/right/bottom、姿态 yaw/pitch/roll，以及原始字节
 * （用于反推字节序，历史结论是 state 为小端但手册未明写）。
 * 默认关闭：打开后逐帧打印（真机约 2.1 帧/秒）会刷屏，仅诊断期开。
 * 取值语义**白名单式**：只有 1/true/yes/on（大小写不敏感）算开启，其余（含空、"0"、
 * "false"、"no"、乱写）一律关闭——与远程通道 SAFE_ALLOW_INJECT 同一纪律：
 * 诊断开关绝不能因歧义输入被意外打开。 */
static bool     s_note_dbg;
static uint32_t s_note_frames;                              /* 收到的 FACE_STATE 帧数 */
static uint32_t s_note_state_cnt[8];                        /* 按 state 计数（越界归 7） */
static uint32_t s_note_box_nonzero;                         /* 人脸框非全零的帧数 */
static uint32_t s_note_dbg_next_ms;                         /* 下一次汇总打印时刻 */

/* 大小写不敏感的字符串相等（避免引入 strcasecmp 的平台差异） */
static bool ci_eq(const char * a, const char * b)
{
    if(a == NULL || b == NULL) return false;
    while(*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if(cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if(ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool env_flag_on(const char * v)
{
    if(v == NULL) return false;
    while(*v == ' ' || *v == '\t') v++;
    return ci_eq(v, "1") || ci_eq(v, "true") || ci_eq(v, "yes") || ci_eq(v, "on");
}

/* 在途的静默命令（fm225_send_silent 下发：不进 pending、不重发、不报 ERROR）。
 * 它们同样会抢占模组侧进行中的会话，所以必须单独占一个「在途」标记，否则录入
 * 会在 0x24 清单查询还没被消化时就下发，把模组打成「后到的抢占先到的」。
 * 0 = 无在途；非 0 = 命令字（0x10 清场 / 0x24 清单查询 / 0x23 清录入状态）。 */
static uint8_t  s_silent_cmd;
static uint32_t s_silent_at_ms;

static fm225_proto_ctx_t s_proto;

/* 当前 tick 时刻（fm225_tick 的 now_ms 快照）。
 * 用途：ENROLL 应答打日志时要算「出结果耗时」，而应答回调在 face 线程里跑、没有
 * now_ms 参数；这里由主线程在 tick 开头留一份同源于 now_ms 的时刻，与
 * s_enroll_sent_at_ms 相减即为真实等待时长（两者同一时钟基，不会跨基回绕）。
 * 跨线程只写这一个标量、只用于日志，纪律同 s_face_state。 */
static uint32_t s_now_ms;

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
        if(dev == NULL || *dev == '\0') dev = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0";
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

    /* NOTE 明细诊断开关：每次 open（= 每次 start）重新读取并清零计数 */
    s_note_dbg          = env_flag_on(getenv("SAFE_FM225_NOTE_DEBUG"));
    s_note_frames       = 0;
    s_note_box_nonzero  = 0;
    memset(s_note_state_cnt, 0, sizeof(s_note_state_cnt));
    s_note_dbg_next_ms  = 0;
    printf("[fm225] NOTE 明细诊断 %s（env SAFE_FM225_NOTE_DEBUG）\n",
           s_note_dbg ? "开启" : "关闭");
    printf("[fm225] 已打开 %s（fd=%d，115200 8N1）\n", dev, s_uart_fd);
    return SAFE_OK;
}

static void fm225_close(void)
{
    if(s_uart_fd >= 0) close(s_uart_fd);
    s_uart_fd      = -1;
    s_pending_len  = 0;
}

/* 把 UTF-8 名字拷进定长字段，不切断多字节字符（放不下的整字丢弃，末尾补 NUL）。
 * 模组侧 user_name 字段是 32B 定长，中文 3B/字 → 最多 10 字（30B），实际够用。
 * （放在帧下发段最前：录入请求的延迟下发路径也要用它把名字写进 ENROLL 载荷。） */
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

/* 下发一条「不问结果」的命令（0x10 清场 / 0x24 清单查询 / 0x23 清录入状态）。
 * 不走 fm225_send_cmd：那条路径会记 pending 并在超时后重发 + 报 FACE_EV_ERROR，
 * 而清场/查询失败属「尽力而为」，不该在界面弹错误横幅、更不该刷屏重发。
 * ⚠️ 但它**同样会抢占模组侧进行中的会话**（实测 2026-09-15），因此必须占住
 * s_silent_cmd 这个「在途」标记，否则 ENROLL 会在这条命令还没被模组消化完时
 * 就下发 —— 先到的那条会被静默挤掉、无应答。 */
static void fm225_send_silent(uint8_t cmd)
{
    const uint8_t frame[6] = { FM225_SYNC0, FM225_SYNC1, cmd, 0x00, 0x00, cmd };
    if(fm225_send_raw(frame, sizeof(frame)) == SAFE_OK) {   /* XOR = cmd ^ 0 ^ 0 = cmd */
        s_silent_cmd   = cmd;
        s_silent_at_ms = hal_time_ms();
    }
}

/* 模组是否「正被别的会话占用」——会话型命令（ENROLL / VERIFY / DELETE）下发前的判据。
 * 三种占用来源：
 *   ① s_pending_len > 0   有命令已下发、还在等它的 REPLY；
 *   ② 会话状态机在跑       WAIT_VERIFY / WAIT_ENROLL / WAIT_DELETE；
 *   ③ s_silent_cmd != 0    有静默命令在途（它不进 pending，但一样会抢占会话）。
 * ★ FM225_ENROLL_ARMING 不算「被别人占用」——那是本次录入请求自己占的排队位。 */
static bool fm225_module_busy(void)
{
    if(s_pending_len > 0) return true;
    if(s_state == FM225_WAIT_VERIFY || s_state == FM225_WAIT_ENROLL ||
       s_state == FM225_WAIT_DELETE) return true;
    if(s_silent_cmd != 0) return true;
    return false;
}

/* 启动对账第一步：取模组用户清单（FR-21 防线 3）。
 * 只在「空闲且无 pending」时发 —— 实测（2026-09-15）新命令会**抢占**进行中的会话，
 * 且被抢占的会话静默无应答；识别链路优先，查询让路。
 * 新增两条守卫：录入排队/进行中一律不插查询（s_state != FM225_IDLE 已覆盖排队与
 * 进行中两种态），上一条静默查询还没消化完也不重发。 */
static void fm225_recon_poll(uint32_t now_ms)
{
    if(s_mod_id_count >= 0) return;                     /* 已取得，不再查 */
    if(!s_started || s_pending_len > 0 || s_state != FM225_IDLE) return;
    if(s_enroll_req) return;                            /* 录入排队中：不插查询 */
    if(s_silent_cmd != 0) return;                       /* 上一条静默查询还在途 */
    if(s_recon_at_ms != 0 && (now_ms - s_recon_at_ms) < FM225_RECON_RETRY_MS) return;
    s_recon_at_ms = now_ms;
    fm225_send_silent(FM225_CMD_GET_ALL_USERID);
}

/* 录入请求的实际下发点（tick 驱动，见 fm225_enroll 里「为什么要延迟下发」）。
 * 返回 true 表示本次 tick 真的把 ENROLL 打下去了。 */
static bool fm225_enroll_issue(uint32_t now_ms)
{
    if(!s_enroll_req) return false;

    if(!s_started) {                    /* 排队期间被 stop / 离页：丢弃请求 */
        s_enroll_req = false;
        if(s_state == FM225_ENROLL_ARMING) s_state = FM225_IDLE;
        if(s_enroll_arm_at_ms != 0) {
            printf("[fm225] 录入请求作废：后端已停止（未下发 ENROLL）\n");
        }
        return false;
    }
    if(fm225_module_busy()) return false;
    /* 静默窗口未过（有符号比较：now_ms 早于 arm 时刻时差为负，也判为未过） */
    if((int32_t)(now_ms - s_enroll_arm_at_ms) < (int32_t)FM225_SETTLE_MS) return false;

    /* ENROLL 载荷（手册 §六 H>>M，共 35B）：
     *   p[0]     admin     = 0   角色由业务层记录，模组 admin 标记不用（实测 admin=0 被接受）
     *   p[1..32] user_name 本地用户名 —— v1.4（FR-21 防线 1）：把名字写进模组侧模板，
     *                      VERIFY 成功应答会原样带回，供业务层核对凭据归属
     *   p[33]    face_dir  = 0x00（FACE_DIRECTION_UNDEFINE，手册标注「未定义，默认为
     *                      正向」；实测 2026-09-15：0x00 与 0x01(FACE_DIRECTION_MIDDLE)
     *                      行为完全一致，都进入录入流程并在 timeout 后回
     *                      MR_FAILED4_TIMEOUT，故维持 0x00）
     *                      ★ 当前拍板（用户 2026-09-15）：**单帧模式**（一次采集即成模板，
     *                      最简单可靠）；多角度方向录入为后续升级项，勿在本函数顺手加）
     *   p[34]    timeout   = FM225_ENROLL_TIMEOUT_S */
    uint8_t p[35];
    memset(p, 0, sizeof(p));
    copy_name_utf8((char *)(p + 1), 32, s_enroll_name);
    /* 可切换录入模式：单帧 0x00 / 五向 0x1F（见 s_enroll_5way 注释） */
    p[33] = s_enroll_5way ? 0x1F : 0x00;
    /* 超时：五向要依次采集五个朝向，10s 未必够（手册说 timeout 最大 255s），
     * 这里给五向单独一个更宽的值，单帧保持原值不变。 */
    p[34] = s_enroll_5way ? FM225_ENROLL_TIMEOUT_5WAY : FM225_ENROLL_TIMEOUT_S;

    safe_err_t e = fm225_send_cmd(FM225_CMD_ENROLL, p, sizeof(p));
    s_enroll_req = false;

    if(e != SAFE_OK) {
        s_state = FM225_IDLE;
        printf("[fm225] ENROLL 下发失败：err=%d（模组未进入录入）\n", (int)e);
        face_enroll_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_id = -1;
        r.err     = e;
        face_service_emit(FACE_EV_ENROLL_DONE, &r);
        return false;
    }

    s_state              = FM225_WAIT_ENROLL;
    s_enroll_sent_at_ms  = now_ms;
    s_enroll_note_count  = 0;
    s_enroll_last_state  = -1;
    s_enroll_hint_done   = false;
    printf("[fm225] ENROLL 下发：name=\"%s\" dir=0x%02X timeout=%us"
           "（模组已空闲，录入会话独占开始，会话超时 %ums）\n",
           s_enroll_name, (unsigned)p[33], (unsigned)p[34],
           (unsigned)FM225_SESSION_TIMEOUT_MS);
    return true;
}

/* 单次识别（FR-27 拍板）：仅在空闲时受理一次 VERIFY 会话；结果/超时走既有
 * 应答链，结束即静默，不再自动续发。忙 = SAFE_ERR_BUSY。
 * 「空闲」= 无 pending、无会话态（含 ENROLL_ARMING / WAIT_ENROLL —— 录入期间不受理
 * 识别，否则 VERIFY 会把录入会话挤掉）、无在途静默命令。
 * 注：在途静默命令这里**不挡**——清单查询只在启动对账期（s_mod_id_count < 0）出现，
 * 挡住会让该期间的识别按钮间歇性返回 BUSY；真要撞上，被挤掉的只是可重发的查询。 */
static safe_err_t fm225_verify_once(void)
{
    if(!s_started) return SAFE_ERR_STATE;
    if(s_pending_len > 0 || s_state != FM225_IDLE) return SAFE_ERR_BUSY;
    const uint8_t p[] = { 0x00, FM225_VERIFY_TIMEOUT_S };
    safe_err_t e = fm225_send_cmd(FM225_CMD_VERIFY, p, sizeof(p));
    if(e == SAFE_OK) s_state = FM225_WAIT_VERIFY;
    return e;
}

static int32_t fm225_face_state(void)
{
    return s_face_state;
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
            printf("[fm225] 离开人脸页：0x10 MID_RESET 终止会话\n");
        }
        s_pending_len = 0;
        s_state       = FM225_IDLE;
        s_enroll_req  = false;      /* 排队中的录入请求一并作废（模组已被静默） */
    }
    else {
        printf("[fm225] 进入人脸页：恢复识别会话\n");
    }
}

/* 本轮是否「向模组要过东西」—— 只有在这种轮次里，「没收到应答」才是故障信号。
 *
 * 背景（FR-27 后识别改为界面按钮单次触发）：空闲期模组**故意**完全静默，不发任何
 * 帧。若仍按「连续 N 轮无帧」判不健康，空闲 15s 必然误报 —— 真机实测（2026-09-16）
 * 日志就是「模组用户清单：0 个（应答 data 201 字节）」之后紧接着
 * 「模组健康：健康 -> 不健康」，模组明明在答，界面却显示「模组无响应」。
 * 判据因此改成：**没提问，就无所谓无应答**。
 *
 * 计入的三类「有在途期待」：
 *   1) s_pending_len > 0            已下发命令、还没收到它的 REPLY；
 *   2) s_state != FM225_IDLE        会话进行中（录入/验证/删除），模组应持续上报 NOTE；
 *   3) s_mod_id_count < 0           启动对账的 0x24 清单查询还没拿到结果。
 *      ★ 这一项保住「上电即失联」的检测：它对账应答永远不会来，必须能判死。
 * 不计入：FR-27 空闲静默期（对账已完成、无在途命令）。 */
static bool fm225_health_expecting_reply(void)
{
    return (s_pending_len > 0) || (s_state != FM225_IDLE) || (s_mod_id_count < 0);
}

/* 健康监测（主线程 20ms tick 驱动，FR-23，不新增线程）：以 5s 为一轮，连续 3 轮（≈15s）
 * **在向模组要过东西的前提下**没收到任何合法帧 → 不健康；本轮内收到过帧即清零
 * 连无应答计数并立即恢复。空闲静默轮不计数（详见 fm225_health_expecting_reply）。 */
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
    if(!fm225_health_expecting_reply()) return;   /* 空闲静默轮：不计数 */

    if(s_health_miss_rounds < FM225_HEALTH_FAIL_ROUNDS) s_health_miss_rounds++;
    if(s_health_miss_rounds >= FM225_HEALTH_FAIL_ROUNDS) fm225_set_health(FACE_MOD_FAIL);
}

/* ---------------- 帧回调（face 线程内执行） ---------------- */

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

/* MR_* 结果码的可读名（排错日志用）。
 * ⚠️ 只做「结果码数值 → 手册常量名」的翻译，不碰 core 的 safe_err_str —— 本文件在
 * hal 层，hal→core 是反向依赖，链接期会 undefined reference（历史踩过）。
 * 面向用户的语义文案由 UI 层翻译（page_users.c）。 */
static const char * fm225_mr_name(uint8_t mr)
{
    switch(mr) {
        case FM225_MR_SUCCESS:               return "SUCCESS";
        case FM225_MR_REJECTED:              return "REJECTED";
        case FM225_MR_ABORTED:               return "ABORTED";
        case FM225_MR_FAILED4_CAMERA:        return "FAILED4_CAMERA";
        case FM225_MR_FAILED4_UNKNOWN:       return "FAILED4_UNKNOWN";
        case FM225_MR_FAILED4_INVALIDPARAM:  return "FAILED4_INVALIDPARAM";
        case FM225_MR_FAILED4_NOMEMORY:      return "FAILED4_NOMEMORY";
        case FM225_MR_FAILED4_UNKNOWNUSER:   return "FAILED4_UNKNOWNUSER";
        case FM225_MR_FAILED4_MAXUSER:       return "FAILED4_MAXUSER";
        case FM225_MR_FAILED4_FACEENROLLED:  return "FAILED4_FACEENROLLED";
        case FM225_MR_FAILED4_LIVENESS:      return "FAILED4_LIVENESS";
        case FM225_MR_FAILED4_TIMEOUT:       return "FAILED4_TIMEOUT";
        default:                             return "UNKNOWN_MR";
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
            s_silent_cmd   = 0;   /* 模组重启：在途静默命令的应答永远不会来了 */
            printf("[fm225] NOTE READY：模组就绪\n");
        }
        /* NID_FACE_STATE：录入/验证过程中的人脸状态实时引导（FR-19）。实测（真模组）
         * 帧长 17B = nid(1) + s_note_data_face{state,left,top,right,bottom,yaw,pitch,roll}
         * （各 int16、高字节在前，手册 P24），约 2.1 条/秒；无人脸时 state = 1（NOFACE）。
         * 解析 state 存入 volatile 标量供页面实时引导小字消费（face 线程写、主线程读）；
         * 坐标/姿态字段留作后续多角度引导升级。仅状态翻转打一条日志（排错用，不刷屏）。 */
        if(f->mid_or_nid == FM225_NID_FACE_STATE && f->data_len >= 3) {
            /* ⚠ 字节序实测（2026-09-15）：state 是**小端** int16（GET_ALL_USERID 的
             * 用户 ID 才是手册标注的大端）——按大端解析会得到 0x0100 类垃圾值。 */
            int st = (int)(f->data[1] | (f->data[2] << 8));
            /* 录入会话可观测（FR-19 排错）：统计模组在本次会话里到底上报过几条人脸
             * 状态。这条计数是「模组有没有真的在跑录入」的判据 —— 0 条 = 模组大概率
             * 没进录入（命令被抢占 / 没落地）；有若干条却都是 state=1 = 模组在跑、
             * 只是没看见脸。两种情况的处置完全不同，日志必须能区分。 */
            if(s_state == FM225_WAIT_ENROLL) {
                s_enroll_note_count++;
                s_enroll_last_state = st;
            }
            /* 姿态快照：整组刷新（state 已在上文写入 s_face_state）。
             * 与诊断分支同源的小端 int16（实测 2026-09-15），越界字段补 0。 */
            {
                int16_t y = 0, pi = 0, ro = 0;
                if(f->data_len >= 15) {
                    y  = (int16_t)((uint16_t)f->data[11] | ((uint16_t)f->data[12] << 8));
                    pi = (int16_t)((uint16_t)f->data[13] | ((uint16_t)f->data[14] << 8));
                }
                if(f->data_len >= 17) {
                    ro = (int16_t)((uint16_t)f->data[15] | ((uint16_t)f->data[16] << 8));
                }
                s_pose_yaw = y; s_pose_pitch = pi; s_pose_roll = ro;
            }
            if(s_note_dbg) {
                /* 完整解析 LE int16 × 8：v[0]=state, v[1..4]=left/top/right/bottom,
                 * v[5..7]=yaw/pitch/roll。越过 data_len 的字段补 0（不读越界）。
                 * raw 打前 4 字节：字节序若与实现假设不符，这一行即可反推。 */
                int32_t v[8];
                for(int k = 0; k < 8; k++) {
                    size_t o = (size_t)(1 + k * 2);
                    v[k] = (o + 1 < (size_t)f->data_len)
                               ? (int32_t)(int16_t)((uint16_t)f->data[o] | ((uint16_t)f->data[o + 1] << 8))
                               : 0;
                }
                s_note_frames++;
                s_note_state_cnt[(st >= 0 && st < 8) ? (size_t)st : 7u]++;
                if(v[1] || v[2] || v[3] || v[4]) s_note_box_nonzero++;
                printf("[NOTE-DBG] state=%d L=%d T=%d R=%d B=%d yaw=%d pitch=%d roll=%d "
                       "raw=%02X %02X %02X %02X len=%u\n",
                       st, v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                       f->data[0], f->data[1], f->data[2], f->data[3], (unsigned)f->data_len);

                uint32_t ndbg_now = hal_time_ms();
                if((int32_t)(ndbg_now - s_note_dbg_next_ms) >= 0) {
                    s_note_dbg_next_ms = ndbg_now + 5000u;
                    printf("[NOTE-DBG] 汇总：帧=%u 非零框=%u | state 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u\n",
                           (unsigned)s_note_frames, (unsigned)s_note_box_nonzero,
                           (unsigned)s_note_state_cnt[0], (unsigned)s_note_state_cnt[1],
                           (unsigned)s_note_state_cnt[2], (unsigned)s_note_state_cnt[3],
                           (unsigned)s_note_state_cnt[4], (unsigned)s_note_state_cnt[5],
                           (unsigned)s_note_state_cnt[6], (unsigned)s_note_state_cnt[7]);
                }
            }
            if(st != s_face_state) {
                s_face_state = st;
                printf("[fm225] 人脸状态：%s\n",
                       st == 0 ? "正常" : st == 1 ? "未检测到" :
                       st == 2 ? "太靠上" : st == 3 ? "太靠下" :
                       st == 4 ? "太靠左" : st == 5 ? "太靠右" : "未知");
            }
        }
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
        /* 手册 V1.7 1076-1084：REPLY 的 enroll 数据 = user_id_heb + user_id_leb
         * + face_direction。第三个字节就是已完成方向掩码，供 UI 显示进度。 */
        r.face_dir_mask = (f->data_len >= 3) ? f->data[2] : 0;
        /* 同时打原始 MR 码：错误码是给 UI 的语义，MR 码是给排错用的现场。
         * 注意：本文件在 hal 层，不得调 core 的 safe_err_str（hal→core 是反向依赖，
         * 链接期会直接 undefined reference）——只打数值，语义由 UI 层翻译。
         * 一并打出「会话期间模组上报人脸状态条数 / 末次状态 / 出结果耗时」：
         * 真机「录入总是失败」时，这三项直接区分「模组没进录入」（0 条）、
         * 「模组在跑但没看见脸」（有若干条、末次 state=1）与「录入真失败了」。 */
        printf("[fm225] enroll 应答：err=%d mr=0x%02X(%s) uid=%d"
               "（NOTE 人脸状态 %u 条，末次 state=%d，耗时 %ums）\n",
               (int)r.err, f->result, fm225_mr_name(f->result), r.face_id,
               (unsigned)s_enroll_note_count, (int)s_enroll_last_state,
               (unsigned)(s_now_ms - s_enroll_sent_at_ms));
        /* 录入未成功：0x23 FACE RESET 清录入残留状态（手册 §录入流程：「录入过程中
         * 可通过 FACE RESET 指令终止录入，先前的录入状态也会清零」）。
         * ★ 只在失败时发：成功时模组已经出过 REPLY、模板已落库，此时再发 0x23
         * 有把刚写入的模板清掉的风险（收益是臆测的、代价不可逆），故成功路径不动。
         * 失败/超时路径留下的「未完成的录入上下文」正是「下一次录入总是失败」的嫌疑点。 */
        if(r.err != SAFE_OK) {
            fm225_send_silent(FM225_CMD_FACE_RESET);
            printf("[fm225] 录入未成功：0x23 FACE RESET 清除录入残留状态\n");
        }
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
        if(s_silent_cmd == FM225_CMD_GET_ALL_USERID) s_silent_cmd = 0;  /* 静默查询已应答 */
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

    /* 录入请求与静默命令在途标记复位（会话独占相关，见 fm225_module_busy） */
    s_enroll_req        = false;
    s_enroll_name[0]    = '\0';
    s_enroll_arm_at_ms  = 0;
    s_enroll_sent_at_ms = 0;
    s_enroll_note_count = 0;
    s_enroll_last_state = -1;
    s_enroll_hint_done  = false;

    /* 录入模式：env SAFE_FACE_ENROLL_5WAY 切五向。
     * 白名单式（只认 1/true/yes/on，其余一律关）—— 与 SAFE_FM225_NOTE_DEBUG 等
     * 既有开关同一纪律，防止写成 =false 反而开启的脚枪。 */
    {
        const char * e = getenv("SAFE_FACE_ENROLL_5WAY");
        s_enroll_5way = (e != NULL &&
                         (strcmp(e, "1") == 0 || strcmp(e, "true") == 0 ||
                          strcmp(e, "yes") == 0  || strcmp(e, "on") == 0));
    }
    s_pose_yaw = 0; s_pose_pitch = 0; s_pose_roll = 0;
    s_silent_cmd        = 0;
    s_silent_at_ms      = 0;

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
    /* 不在 start 时布防 VERIFY（FR-27 拍板 2026-09-15）：识别由界面按钮单次
     * 触发（face_service_verify_once），其余时间——含启动后——模组静默低功耗。 */
    return SAFE_OK;
}

static safe_err_t fm225_stop(void)
{
    s_started = false;
    s_enroll_req = false;                 /* 排队中的录入请求一并作废 */
    /* MID_RESET(0x10)：通用复位。手册 §3——「模组将取消之前正在执行的命令（例如
     * 录入、解锁等），返回 STANDBY 状态」；实测（2026-09-15 真模组）下发后约 26ms
     * 回 REPLY(result=0x00)，进行中的会话立即停止出 NOTE（静默终止，不回 ABORTED）。
     * ★ 这里**不用** 0x23 FACE RESET：手册里 0x23 只清「录入状态」，取消不掉在飞的
     * VERIFY；stop 要的是「把模组拉回 STANDBY」，只有 0x10 做得到。
     *   0x23 的用途见 ENROLL 应答分支（录入失败后清录入残留状态）。
     * 术语订正：本行原注释把 0x10 称作「FACE RESET」，属手册术语混用——0x10 是
     * MID_RESET，MID_FACERESET(0x23) 才是 FACE RESET。 */
    fm225_send_cmd(FM225_CMD_RESET, NULL, 0);
    s_state        = FM225_IDLE;
    s_pending_len  = 0;
    s_silent_cmd   = 0;
    return SAFE_OK;
}

/* 会话型命令（VERIFY / ENROLL）到点仍无应答：放弃本轮，**不重发**（理由见 fm225_tick）。
 *
 * 「被抢占方静默无应答」是真模组的实测行为（2026-09-15），所以这条路径不是异常，
 * 而是必须能兜住的正常分支 —— 兜不住的话界面就一直停在「录入中」，只能等
 * face_service 的 30s 外层兜底（ENROLL_TIMEOUT_MS）才动，且拿不到任何现场信息。
 * 兜底出口：ENROLL 走 FACE_EV_ENROLL_DONE(SAFE_ERR_TIMEOUT)（顺带把 s_enroll_pending
 * 清掉，UI 立即退出等待态）；VERIFY 走 FACE_EV_ERROR。 */
static void fm225_session_giveup(void)
{
    const uint8_t cmd = s_pending_cmd;
    s_pending_len = 0;
    s_state       = FM225_IDLE;

    /* 模组大概率还卡在被抢占 / 没起来的会话里：0x10 MID_RESET 把它拉回 STANDBY
     * （手册 §3）。清场命令自己不应答也无所谓，故走静默下发。 */
    fm225_send_silent(FM225_CMD_RESET);

    if(cmd == FM225_CMD_ENROLL) {
        printf("[fm225] ENROLL 会话超时：%ums 无应答，放弃本轮（不重发）——"
               "疑似 ENROLL 被抢占或命令未落地；会话期间模组上报人脸状态 %u 条"
               "（末次 state=%d）\n",
               (unsigned)FM225_SESSION_TIMEOUT_MS,
               (unsigned)s_enroll_note_count, (int)s_enroll_last_state);
        face_enroll_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_id = -1;
        r.err     = SAFE_ERR_TIMEOUT;
        face_service_emit(FACE_EV_ENROLL_DONE, &r);
    }
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "fm225 命令 0x%02X 无应答（会话型命令不重发）",
                 (unsigned)cmd);
        printf("[fm225] %s\n", msg);
        face_service_emit(FACE_EV_ERROR, msg);
    }
}

static void fm225_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;   /* 留一份给 face 线程的应答回调算耗时用（见 s_now_ms 注释） */

    /* 协议状态机半帧超时推进（feed 在 face 线程，tick 在主线程，见 §5.20 线程模型） */
    fm225_proto_tick(&s_proto, now_ms);

    /* Duty-cycle 边沿（FR-27）：离开人脸页终止会话、进入时恢复 */
    fm225_fg_tick(now_ms);

    /* 模组健康监测（FR-23）：复用主线程 20ms tick，不新增线程（内部有前台守卫） */
    fm225_health_tick(now_ms);

    /* 前台暂停：不对账 / 不重发 / 不重开识别会话 */
    if(!face_service_foreground()) return;

    /* 静默命令（0x10 清场 / 0x24 清单查询 / 0x23 清录入状态）的在途窗口：到点即视为
     * 结束，解除「在途」标记。它们不进 pending、没有超时重发，必须有个兜底出口——
     * 否则模组永不应答 0x24 时 s_silent_cmd 会一直占着，ENROLL 永远排不上队。 */
    if(s_silent_cmd != 0 &&
       (int32_t)(now_ms - s_silent_at_ms) >= (int32_t)FM225_SETTLE_MS) {
        s_silent_cmd = 0;
    }

    /* 录入请求下发：排在 recon 之前——录入优先，且下发成功后 recon 会被会话独占拦住 */
    fm225_enroll_issue(now_ms);

    /* 启动对账：空闲时取一次模组用户清单（FR-21 防线 3） */
    fm225_recon_poll(now_ms);

    /* 录入中的人脸状态可观测提示（FR-19）：会话跑过 FM225_ENROLL_HINT_MS 模组还没出过
     * 一条 NOTE 人脸状态，说明模组大概率**根本没进录入**（ENROLL 被抢占 / 没落地），
     * 而不是「人没站到镜头前」。这两种情况的处置完全不同，日志必须能当场区分。 */
    if(s_state == FM225_WAIT_ENROLL && !s_enroll_hint_done &&
       (int32_t)(now_ms - s_enroll_sent_at_ms) >= (int32_t)FM225_ENROLL_HINT_MS) {
        s_enroll_hint_done = true;
        if(s_enroll_note_count == 0) {
            printf("[fm225] 录入已进行 %ums：模组尚未上报任何人脸状态（NOTE）——"
                   "模组可能未进入录入（ENROLL 被抢占 / 命令未落地），"
                   "而非「没站到镜头前」\n", (unsigned)FM225_ENROLL_HINT_MS);
        }
    }

    /* 命令应答超时处理。now_ms 与 s_pending_at_ms 同为 hal_time_ms() 同源单调毫秒，
     * 差值才是真实等待时长（D1 前：20ms 自增计数 vs 绝对 ms 跨基 → 回绕后恒真）。
     * 超时值按命令分型（D17）：VERIFY/ENROLL 是会话命令用 SESSION 超时，
     * 其余（RESET/GETSTATUS/DELETE）立即应答用 800ms。
     * ★ D18：会话型命令**不重发**。真模组实测「新命令会抢占进行中的会话，被抢占方
     *   静默无应答」——重发 ENROLL 只会把模组里正在跑的那一轮重新抢占、10s 计时从头
     *   再来（3 次重发 = 界面空转 36s），且到底哪一轮成功彻底不可判定。改为到点直接
     *   放弃并上报（ENROLL → FACE_EV_ENROLL_DONE(TIMEOUT)；VERIFY → FACE_EV_ERROR），
     *   由用户/上层重新发起，比「看起来在重试其实在互相踩」诚实。 */
    const bool     session_cmd = (s_pending_cmd == FM225_CMD_VERIFY ||
                                  s_pending_cmd == FM225_CMD_ENROLL);
    const uint32_t pending_timeout_ms = session_cmd ? FM225_SESSION_TIMEOUT_MS
                                                    : FM225_REPLY_TIMEOUT_MS;
    if(s_pending_len > 0 && s_started &&
       (now_ms - s_pending_at_ms) > pending_timeout_ms) {
        if(session_cmd) {
            fm225_session_giveup();
        }
        else if(s_retry >= FM225_MAX_RETRY) {
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

    /* verify 不再自动续发（FR-27 拍板 2026-09-15）：识别改为界面按钮单次触发
     *（face_service_verify_once → fm225_verify_once），会话结束即静默低功耗。
     * 原 REARM 自动循环随按键触发模式一并移除；重发放弃后由用户再次发起。 */
}

static safe_err_t fm225_enroll(const char * user_name)
{
    if(!s_started) return SAFE_ERR_STATE;
    /* 已有录入在途（排队中 or 已下发）→ 忙，不再叠一轮 */
    if(s_enroll_req || s_state == FM225_ENROLL_ARMING || s_state == FM225_WAIT_ENROLL)
        return SAFE_ERR_BUSY;

    /* 清场：模组可能还有在途会话（VERIFY 在飞 / 上一条静默命令还没消化完）。
     * 用 0x10 MID_RESET —— 手册 §3：「模组将取消之前正在执行的命令（例如录入、
     * 解锁等），返回 STANDBY 状态」。它是两个候选里唯一同时覆盖「录入」和「解锁」
     * 的：0x23 FACE RESET（手册 §录入流程）只清录入状态，取消不掉在飞的 VERIFY。
     * 清场后仍要等 FM225_SETTLE_MS 静默窗口才真正下发 ENROLL（见 fm225_enroll_issue）。
     * 被清掉的那条命令按实测行为静默无应答，故直接作废 pending，不等它的 REPLY。 */
    if(fm225_module_busy()) {
        const unsigned pend = (unsigned)s_pending_len;
        const int      st   = (int)s_state;
        const unsigned sil  = (unsigned)s_silent_cmd;
        fm225_send_silent(FM225_CMD_RESET);
        s_pending_len = 0;
        s_state       = FM225_IDLE;
        printf("[fm225] 录入受理：模组忙（pending=%u state=%d silent=0x%02X）"
               "→ 0x10 MID_RESET 清场\n", pend, st, sil);
    }

    /* ★ 受理与下发分离（本轮核心修复）：
     * 真模组实测（2026-09-15）——**新命令会抢占进行中的会话，且被抢占方静默无应答**。
     * 旧实现在这里直接 fm225_send_cmd(ENROLL)：若此时刚好有 VERIFY 在飞、或 0x24
     * 清单查询刚发出，ENROLL 会把它们挤掉；若反过来（ENROLL 先发、清场命令后到），
     * ENROLL 自己被挤掉、界面就一直停在「录入中」直到 30s 外层兜底超时。
     * 现在：受理立刻返回 SAFE_OK（UI 进「录入中」），真正下发交给 tick，条件是
     * 「模组完全空闲（无 pending / 无其它会话 / 无在途静默命令）+ 静默窗口已过」。
     * 代价：最坏多等 300ms。收益：录入会话独占，不再与任何命令互相抢占。 */
    copy_name_utf8(s_enroll_name, sizeof(s_enroll_name), user_name);
    s_enroll_req       = true;
    s_enroll_arm_at_ms = hal_time_ms();
    s_state            = FM225_ENROLL_ARMING;
    printf("[fm225] 录入受理：name=\"%s\"（等模组空闲 + %ums 静默窗口后下发 ENROLL）\n",
           s_enroll_name, (unsigned)FM225_SETTLE_MS);
    return SAFE_OK;
}

static safe_err_t fm225_delete_tpl(int32_t face_id)
{
    if(face_id < 0) return SAFE_ERR_PARAM;
    /* 会话独占（与 ENROLL 同理：删除也是会话型命令，同样会抢占进行中的会话）。
     * 旧实现只挡「正在删除」这一态，于是录入/验证在途时下发的 DELETE 会把 ENROLL
     * 或 VERIFY 静默挤掉 —— 挤掉的那一方永不应答，界面只能等外层兜底超时。 */
    if(s_state != FM225_IDLE || s_pending_len > 0) return SAFE_ERR_BUSY;
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
    .verify_once  = fm225_verify_once,
    .enroll        = fm225_enroll,
    .delete_tpl    = fm225_delete_tpl,
    .module_users  = fm225_module_users,
    .module_health = fm225_module_health,
    .face_state   = fm225_face_state,
    .inject        = fm225_inject,
};

const face_backend_t * face_backend_fm225(void)
{
    return &backend;
}

