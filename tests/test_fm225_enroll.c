/**
 * @file test_fm225_enroll.c
 * FM225 后端「录入会话独占 + 静默无应答兜底」单元测试。
 *
 * 背景（真机故障「录入人脸总是失败」）：真模组实测（2026-09-15）——**新命令会抢占
 * 进行中的会话，且被抢占方静默无应答**。旧实现在 fm225_enroll() 里直接下发 ENROLL，
 * 于是「点录入」时若刚好有 VERIFY 在飞、或 0x24 清单查询刚发出，两条命令会互相踩；
 * 会话型命令超时后又按即时命令那套重发 3 次，把模组里正在跑的录入反复重启。
 *
 * 本测试锁住新增的四条行为（改坏了就会红）：
 *   1) 录入受理与下发分离：enroll() 只受理，ENROLL 帧要等「模组空闲 + 静默窗口」；
 *   2) 录入会话期间独占：VERIFY / DELETE / 第二次 ENROLL 一律 SAFE_ERR_BUSY，
 *      且对账查询（0x24）不得插入；
 *   3) 会话型命令不重发：ENROLL 下发后不管过多久都只有 1 条 0x13 帧；
 *   4) 静默无应答兜底：模组永不回应答时，后端在会话超时点放弃并上报
 *      FACE_EV_ENROLL_DONE(SAFE_ERR_TIMEOUT)，而不是挂到外层 30s 兜底；
 *      录入以失败收尾时用 0x23 FACE RESET 清录入残留状态（成功时**不**发）。
 *
 * 怎么脱离真机测：
 *   - 用 posix_openpt() 造一对 pty，把 slave 名字塞进 SAFE_FM225_DEV —— 后端照常
 *     open/termios，我们作为「模组侧」从 master 读它下发的帧、用 fm225_backend_feed()
 *     把应答帧灌回去（等价于 face 线程的读路径）；
 *   - 时间用「快进」：fm225_tick(now_ms) 的入参允许传 hal_time_ms() + 快进量，
 *     而后端内部记账用的是 hal_time_ms()，两者差值即「已经等了多久」——于是不用真睡
 *     12 秒就能触发会话超时（这没有污染生产代码：后端契约本来就要求 now_ms 与
 *     hal_time_ms() 同源，快进只是把它往前拨）。
 */
/* posix_openpt / grantpt / unlockpt / ptsname 都要求 XOPEN2K 特性宏，
 * 必须在所有 include 之前定义（否则 glibc 默认头不给声明）。 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "test_util.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/event_bus.h"
#include "hal/face/face_backend.h"
#include "hal/face/fm225_proto.h"
#include "hal/hal_time.h"

/* 后端提供的字节流入口（face 线程真实调用的就是它）。无公开头文件，本地声明。 */
void fm225_backend_feed(const uint8_t * buf, size_t n);
/* 后端持有的串口 fd（face 线程 poll 的就是它）。 */
int fm225_uart_fd(void);

/* ---------------- 假模组接线 ---------------- */

static int      g_ptym = -1;      /* pty master：读 = 后端下发的帧 */
static uint32_t g_adv  = 0;       /* 时间快进量（ms） */
static const face_backend_t * g_b = NULL;

static uint8_t g_rx[8192];        /* 后端下发字节流（累积） */
static size_t  g_rx_n;

static void t_tick(void)
{
    g_b->tick(hal_time_ms() + g_adv);
}

/* 把后端已经写出来的字节收进 g_rx（非阻塞，收干为止）。 */
static void t_drain(void)
{
    struct pollfd pfd;
    pfd.fd      = g_ptym;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    if(poll(&pfd, 1, 20) <= 0) return;          /* 20ms 内没字节 = 后端没下发 */

    for(;;) {
        uint8_t tmp[512];
        ssize_t n = read(g_ptym, tmp, sizeof(tmp));
        if(n <= 0) break;                        /* EAGAIN / EOF */
        for(ssize_t i = 0; i < n && g_rx_n < sizeof(g_rx); i++) g_rx[g_rx_n++] = tmp[i];
    }
}

/* ---------------- 下发帧解析（复用真实协议解析器） ---------------- */

#define LOG_MAX   48
#define LOG_DATA  48

typedef struct {
    uint8_t  msgid;
    uint8_t  data[LOG_DATA];
    uint16_t data_len;
} logframe_t;

typedef struct {
    logframe_t f[LOG_MAX];
    int        n;
} txlog_t;

static void on_tx(const fm225_frame_t * f, void * user)
{
    txlog_t * t = (txlog_t *)user;
    if(t == NULL || t->n >= LOG_MAX) return;
    logframe_t * d = &t->f[t->n++];
    memset(d, 0, sizeof(*d));
    d->msgid    = f->msgid;               /* H>>M 帧：MsgID 就是命令字 */
    d->data_len = f->data_len;
    uint16_t k = f->data_len < LOG_DATA ? f->data_len : (uint16_t)LOG_DATA;
    if(f->data != NULL && k > 0) memcpy(d->data, f->data, k);
}

/* 解析 g_rx 里到目前为止的全部下发帧 */
static void t_parse(txlog_t * t)
{
    memset(t, 0, sizeof(*t));
    fm225_proto_ctx_t c;
    fm225_proto_init(&c, on_tx, t);
    fm225_proto_feed(&c, g_rx, g_rx_n);
}

static int t_count(const txlog_t * t, uint8_t cmd)
{
    int k = 0;
    for(int i = 0; i < t->n; i++) if(t->f[i].msgid == cmd) k++;
    return k;
}

/* 该命令字第一次出现的下标，-1 = 从未下发 */
static int t_first(const txlog_t * t, uint8_t cmd)
{
    for(int i = 0; i < t->n; i++) if(t->f[i].msgid == cmd) return i;
    return -1;
}

/* ---------------- 模组侧应答注入 ---------------- */

/* 构造一帧（与 test_fm225_proto.c 同款） */
static size_t t_frame(uint8_t * out, uint8_t msgid, const uint8_t * data, uint16_t len)
{
    size_t k = 0;
    out[k++] = FM225_SYNC0;
    out[k++] = FM225_SYNC1;
    out[k++] = msgid;
    out[k++] = (uint8_t)(len >> 8);
    out[k++] = (uint8_t)(len & 0xFF);
    uint8_t x = msgid ^ (uint8_t)(len >> 8) ^ (uint8_t)(len & 0xFF);
    for(uint16_t i = 0; i < len; i++) { out[k++] = data[i]; x ^= data[i]; }
    out[k++] = x;
    return k;
}

/* 注入一条 M>>H REPLY：mid + result + 可选业务数据 */
static void t_reply(uint8_t mid, uint8_t result, const uint8_t * extra, uint16_t extra_len)
{
    uint8_t data[256];
    data[0] = mid;
    data[1] = result;
    if(extra != NULL && extra_len > 0) memcpy(data + 2, extra, extra_len);
    uint8_t raw[300];
    size_t n = t_frame(raw, FM225_MSGID_REPLY, data, (uint16_t)(2 + extra_len));
    fm225_backend_feed(raw, n);
}

/* ---------------- 事件总线捕获 ---------------- */

static ev_face_event_t g_evs[16];
static int             g_ev_n;

static void on_face_ev(ev_topic_t topic, const void * payload, void * user)
{
    (void)topic; (void)user;
    if(payload == NULL) return;
    if(g_ev_n < 16) g_evs[g_ev_n++] = *(const ev_face_event_t *)payload;
}

static void t_pump(void)
{
    event_bus_pump();
}

/* 找一条 ENROLL_DONE 事件；无则 NULL */
static const ev_face_event_t * t_find_enroll_done(void)
{
    for(int i = 0; i < g_ev_n; i++)
        if(g_evs[i].ev == FACE_EV_ENROLL_DONE) return &g_evs[i];
    return NULL;
}

static const ev_face_event_t * t_find_error(void)
{
    for(int i = 0; i < g_ev_n; i++)
        if(g_evs[i].ev == FACE_EV_ERROR) return &g_evs[i];
    return NULL;
}

/* 后端复位到「刚上电」：deinit 再 init/start，静态状态全清（含清单 -1、录入请求） */
static void t_reset_backend(void)
{
    g_b->deinit();
    CHECK(g_b->init() == SAFE_OK);
    CHECK(g_b->start() == SAFE_OK);
    g_rx_n = 0;
    g_adv  = 0;
    g_ev_n = 0;
    event_bus_reset();
    event_bus_subscribe(EV_FACE_EVENT, on_face_ev, NULL);
}

/* ---------------- 用例 1：录入受理≠下发，且录入期间独占 ---------------- */

static void test_enroll_exclusive(void)
{
    t_reset_backend();

    /* 启动对账：空闲时后端会先发一条 0x24 清单查询（静默命令，在途） */
    t_tick();
    t_drain();
    txlog_t t;
    t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_GET_ALL_USERID) == 1);

    /* 点「录入人脸」：只受理，不下发（此时 0x24 还在途） */
    CHECK(g_b->enroll("alice") == SAFE_OK);
    t_tick();                                   /* 受理后第一个 tick：静默窗口未过 */
    t_drain();
    t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 0);  /* ★ 没把 ENROLL 插到在途命令上 */
    CHECK(t_count(&t, FM225_CMD_RESET) == 1);   /* 清场：0x10 MID_RESET */

    /* 静默窗口未过（100ms < 300ms）：仍不下发 */
    g_adv += 100;
    t_tick(); t_drain(); t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 0);

    /* 窗口过了（500ms）：模组已空闲 → 下发，且只此一条 */
    g_adv += 400;
    t_tick(); t_drain(); t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 1);
    /* ENROLL 必须排在清场命令之后（顺序即「先清场再录入」） */
    CHECK(t_first(&t, FM225_CMD_ENROLL) > t_first(&t, FM225_CMD_RESET));

    /* 载入会话期间：其它会话型命令一律不受理（否则会互相抢占） */
    CHECK(g_b->verify_once()  == SAFE_ERR_BUSY);
    CHECK(g_b->delete_tpl(1)  == SAFE_ERR_BUSY);
    CHECK(g_b->enroll("dup")  == SAFE_ERR_BUSY);

    /* 会话进行中（快进 4s，未到 12s 会话超时）：
     *   - 对账查询（0x24）不得插入（s_mod_id_count 仍是 -1，它很想发）；
     *   - ENROLL 不得重发。 */
    g_adv += 4000;
    t_tick(); t_drain(); t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_GET_ALL_USERID) == 1);
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 1);
}

/* ---------------- 用例 2：静默无应答兜底（不重发 + 超时上报） ---------------- */

static void test_enroll_silent_no_reply(void)
{
    txlog_t t;

    /* 紧接用例 1：ENROLL 已下发、模组一直不答 */
    g_ev_n = 0;
    g_adv += 9000;                               /* 累计约 13s > 会话超时 12s */
    t_tick(); t_drain(); t_pump();
    t_parse(&t);

    /* ★ 不重发：从下发到放弃，0x13 始终只有 1 条 */
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 1);
    /* 放弃时把模组拉回 STANDBY 的 0x10（用例 1 的清场 1 条 + 本条） */
    CHECK(t_count(&t, FM225_CMD_RESET) == 2);

    /* ★ 兜底上报：ENROLL_DONE(err=TIMEOUT)，而不是挂死到外层 30s */
    const ev_face_event_t * e = t_find_enroll_done();
    CHECK(e != NULL);
    if(e != NULL) {
        CHECK(e->enroll.err == SAFE_ERR_TIMEOUT);
        CHECK(e->enroll.face_id == -1);
    }

    /* 放弃后状态回到空闲：可以再次受理录入（不会永久 BUSY） */
    CHECK(g_b->enroll("again") == SAFE_OK);
}

/* ---------------- 用例 3：ENROLL 载荷 + 失败收尾发 0x23 清录入状态 ---------------- */

static void test_enroll_payload_and_face_reset(void)
{
    t_reset_backend();

    /* 先把对账答掉，免得 0x24 干扰本用例的帧统计 */
    t_tick();
    t_drain();
    {
        const uint8_t body[1] = { 0x00 };        /* user_counts = 0 */
        t_reply(FM225_CMD_GET_ALL_USERID, FM225_MR_SUCCESS, body, 1);
    }
    g_rx_n = 0;

    CHECK(g_b->enroll("carol") == SAFE_OK);
    g_adv += 500;
    t_tick(); t_drain();
    txlog_t t;
    t_parse(&t);
    CHECK(t_count(&t, FM225_CMD_ENROLL) == 1);

    /* 载荷校验（手册 §六 H>>M：admin + user_name[32] + face_dir + timeout = 35B） */
    int idx = t_first(&t, FM225_CMD_ENROLL);
    CHECK(idx >= 0);
    if(idx >= 0) {
        const logframe_t * f = &t.f[idx];
        CHECK(f->data_len == 35);
        CHECK(f->data[0] == 0x00);                       /* admin */
        CHECK(memcmp(f->data + 1, "carol", 5) == 0);     /* user_name */
        CHECK(f->data[33] == 0x00);                      /* face_dir = 默认正向 */
        CHECK(f->data[34] == 10);                        /* timeout = 10s */
    }

    /* 模组回「录入超时」（真机故障现场的那个码：mr=0x0D FAILED4_TIMEOUT） */
    g_ev_n = 0;
    t_reply(FM225_CMD_ENROLL, FM225_MR_FAILED4_TIMEOUT, NULL, 0);
    t_drain(); t_pump();
    t_parse(&t);

    /* 失败收尾：0x23 FACE RESET 清录入残留状态（手册 §录入流程） */
    CHECK(t_count(&t, FM225_CMD_FACE_RESET) == 1);
    /* 结果上报：err 由 mr 映射，0x0D → SAFE_ERR_TIMEOUT */
    const ev_face_event_t * e = t_find_enroll_done();
    CHECK(e != NULL);
    if(e != NULL) {
        CHECK(e->enroll.err == SAFE_ERR_TIMEOUT);
        CHECK(e->enroll.face_id == -1);
    }
}

/* ---------------- 用例 4：录入成功时**不**发 0x23（别把刚写进去的模板清掉） ---------------- */

static void test_enroll_success_no_face_reset(void)
{
    t_reset_backend();
    t_tick();
    t_drain();
    {
        const uint8_t body[1] = { 0x00 };
        t_reply(FM225_CMD_GET_ALL_USERID, FM225_MR_SUCCESS, body, 1);
    }
    g_rx_n = 0;

    CHECK(g_b->enroll("dave") == SAFE_OK);
    g_adv += 500;
    t_tick(); t_drain();

    g_ev_n = 0;
    {
        const uint8_t uid[2] = { 0x00, 0x07 };          /* user_id = 7 */
        t_reply(FM225_CMD_ENROLL, FM225_MR_SUCCESS, uid, 2);
    }
    t_drain(); t_pump();
    txlog_t t;
    t_parse(&t);

    CHECK(t_count(&t, FM225_CMD_ENROLL) == 1);
    CHECK(t_count(&t, FM225_CMD_FACE_RESET) == 0);      /* ★ 成功路径不发 0x23 */

    const ev_face_event_t * e = t_find_enroll_done();
    CHECK(e != NULL);
    if(e != NULL) {
        CHECK(e->enroll.err == SAFE_OK);
        CHECK(e->enroll.face_id == 7);
    }
}

/* ---------------- 用例 5：VERIFY 会话同样不重发，超时走 ERROR 上报 ---------------- */

static void test_verify_session_no_retry(void)
{
    t_reset_backend();
    t_tick();
    t_drain();
    {
        const uint8_t body[1] = { 0x00 };
        t_reply(FM225_CMD_GET_ALL_USERID, FM225_MR_SUCCESS, body, 1);
    }
    g_rx_n = 0;

    CHECK(g_b->verify_once() == SAFE_OK);
    g_adv += 13000;                                     /* 越过会话超时 */
    t_tick(); t_drain(); t_pump();
    txlog_t t;
    t_parse(&t);

    CHECK(t_count(&t, FM225_CMD_VERIFY) == 1);          /* ★ 没重发 */
    const ev_face_event_t * e = t_find_error();
    CHECK(e != NULL);
}

/* ---------------- 实时联调模式（socat + Python 假模组，见 tools/fm225_enroll_sim.py） ----------------
 *
 * 用 FM225_ENROLL_LIVE=1 且 SAFE_FM225_DEV 已指向 socat 假串口时启用：
 * 本程序退化成「真实主机应用」，用真实时间跑一次录入；模组侧的行为由外部假模组
 * 脚本决定（答 / 不答 / 中途插查询）。断言放在假模组脚本侧（它看得见整条线上
 * 的字节），本程序只负责一件事：**必须在会话超时后拿到 ENROLL_DONE，不能永久挂起**。
 *
 * 用法（由 tools/fm225_enroll_sim.py 调用，也可手跑）：
 *   SAFE_FM225_DEV=/tmp/fm225_host FM225_ENROLL_LIVE=1 ./bin/test_fm225_enroll
 * 退出码 0 = 拿到结果（未挂起）；1 = 超时没拿到。
 */

static void live_sleep_ms(int ms)
{
    struct pollfd pfd;
    pfd.fd      = -1;
    pfd.events  = 0;
    pfd.revents = 0;
    (void)poll(&pfd, 0, ms);
}

/* 串口收字节 → 喂协议状态机：与 face_thread.c 的 face_uart_drain() 等价。
 * 这里不开 face 线程（它会把相机 fd 也拉进 poll 集合，牵扯未初始化的相机后端），
 * 只把「读串口 → fm225_backend_feed」这一条链路补上，足够驱动后端状态机。 */
static void live_pump_uart(void)
{
    int fd = fm225_uart_fd();
    if(fd < 0) return;
    uint8_t buf[512];
    for(;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if(n <= 0) break;                      /* EAGAIN（后端以 O_NONBLOCK 打开）/ EOF */
        fm225_backend_feed(buf, (size_t)n);
    }
}

static int live_main(void)
{
    uint32_t t0 = hal_time_ms();

    CHECK(g_b->init() == SAFE_OK);
    CHECK(g_b->start() == SAFE_OK);

    /* 0.5s 后点「录入人脸」（先让启动对账的 0x24 打出去，制造「不空闲」的现实条件） */
    for(int i = 0; i < 25; i++) {
        g_b->tick(hal_time_ms());
        live_pump_uart();
        live_sleep_ms(20);
    }

    safe_err_t rc = g_b->enroll("live");
    printf("[LIVE] enroll() 受理返回 %d（0 = 已受理）\n", (int)rc);

    /* 录入进行中：识别 / 删除必须被拒（会话独占），否则会互相抢占 */
    live_sleep_ms(1500);
    live_pump_uart();
    printf("[LIVE] 录入中 verify_once()=%d delete_tpl()=%d（-10 = BUSY 符合预期）\n",
           (int)g_b->verify_once(), (int)g_b->delete_tpl(1));

    /* 等结果：会话超时 12s + 余量。拿到 ENROLL_DONE 即算没挂起。 */
    const ev_face_event_t * done = NULL;
    for(int i = 0; i < 1100 && done == NULL; i++) {   /* 1100 × 20ms = 22s */
        g_b->tick(hal_time_ms());
        live_pump_uart();
        event_bus_pump();
        done = t_find_enroll_done();
        if(done == NULL) live_sleep_ms(20);
    }

    if(done == NULL) {
        printf("[LIVE] 结果：超时未拿到 ENROLL_DONE（应用层挂起了）\n");
        return 1;
    }
    printf("[LIVE] 结果：ENROLL_DONE err=%d face_id=%d，耗时 %ums（未挂起）\n",
           (int)done->enroll.err, (int)done->enroll.face_id,
           (unsigned)(hal_time_ms() - t0));
    return 0;
}

/* ---------------- main ---------------- */

int main(void)
{
    /* 实时联调模式：用外部 socat 假串口，不自建 pty（假模组在另一端） */
    if(getenv("FM225_ENROLL_LIVE") != NULL) {
        if(getenv("SAFE_FM225_DEV") == NULL) {
            printf("  FAIL FM225_ENROLL_LIVE 需要 SAFE_FM225_DEV 指向 socat 假串口\n");
            return 1;
        }
        g_b = face_backend_fm225();
        if(g_b == NULL) { printf("  FAIL fm225 后端未编入\n"); return 1; }
        event_bus_reset();
        event_bus_subscribe(EV_FACE_EVENT, on_face_ev, NULL);
        return live_main();
    }

    /* 造一对 pty：slave 给后端当串口，master 归我们当「模组侧」 */
    g_ptym = posix_openpt(O_RDWR | O_NOCTTY);
    if(g_ptym < 0) {
        printf("  FAIL posix_openpt 失败\n");
        return 1;
    }
    if(grantpt(g_ptym) != 0 || unlockpt(g_ptym) != 0) {
        printf("  FAIL grantpt/unlockpt 失败\n");
        return 1;
    }
    char * slave_name = ptsname(g_ptym);
    if(slave_name == NULL) {
        printf("  FAIL ptsname 返回 NULL\n");
        return 1;
    }
    if(setenv("SAFE_FM225_DEV", slave_name, 1) != 0) {
        printf("  FAIL setenv 失败\n");
        return 1;
    }
    /* master 设非阻塞：t_drain 收干即可返回，不会卡住 */
    int fl = fcntl(g_ptym, F_GETFL, 0);
    if(fl >= 0) (void)fcntl(g_ptym, F_SETFL, fl | O_NONBLOCK);

    g_b = face_backend_fm225();
    if(g_b == NULL) {
        printf("  FAIL fm225 后端未编入\n");
        return 1;
    }

    event_bus_reset();
    event_bus_subscribe(EV_FACE_EVENT, on_face_ev, NULL);

    CHECK(g_b->init() == SAFE_OK);
    CHECK(g_b->start() == SAFE_OK);

    test_enroll_exclusive();
    test_enroll_silent_no_reply();
    test_enroll_payload_and_face_reset();
    test_enroll_success_no_face_reset();
    test_verify_session_no_retry();

    g_b->deinit();

    (void)close(g_ptym);
    TEST_RESULT();
}
