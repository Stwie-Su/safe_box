/**
 * @file mqtt_client.c
 * MQTT 客户端实现（R9 起自研：单线程 poll + 非阻塞连接 + 自研状态机）。
 *
 * ★ 为什么要自研（这是 R9 的业务价值，面试要能一句话讲清）：
 *   板子上**没有可用的 ARM 版第三方 MQTT 库** ⇒ 交叉编译时依赖探测为假 ⇒
 *   编 mqtt_stub.c ⇒ 板上远程通道**永久断线**。不是为了造轮子，
 *   是因为根本没有可用的轮子。
 *   附带收益：原第三方库的异步 API 内部占 2 条线程，自研后只占 1 条 ——
 *   进程内线程数净减 1。
 *
 * 线程模型（应用**第 4 条**自建线程，正好用满规约 §3.3 的 ≤4 预算）：
 *   单线程 poll() 循环 + 非阻塞 connect + self-pipe 唤醒 + 两条定长静态队列。
 *   为什么不能收敛进 worker / face / 主线程（规约要求新增线程必须给理由）：
 *     · worker：阻塞在条件变量等作业，**没有 poll 循环**；且一个 500ms 的落盘作业
 *       会让 MQTT 的 PING 超时误判（定时器没法按时推进）。
 *     · face：它的 poll 是取帧实时性敏感（预览 fps），相机 + FM225 串口已占满节拍；
 *       且 TLS 握手是长阻塞计算，会把预览冻住。
 *     · 主线程：LVGL 20ms 节拍，重连退避最长 30s，扛不住。
 *
 * 背压红线（规约 §5.10.1）：下行队列满时**不入队 + 不发 PUBACK** → broker 判定
 *   QoS1 未确认后重投（DUP=1）。★ 绝不静默丢指令。且背压**绝不能**靠「停止读套接字」
 *   实现 —— 那会关死 TCP 窗口 → 读不到 PINGRESP → keepalive 超时 → 重连风暴。
 *
 * 对外契约：mqtt_client.h 的既有 8 个签名一个字没改（rpc.c / ui.c 依赖）。
 */
#include "core/remote/mqtt_client.h"

#include "core/remote/mqtt_codec.h"
#include "core/remote/mqtt_inflight.h"
#include "core/remote/mqtt_fsm.h"
#include "core/remote/tls_stream.h"
#include "hal/hal_time.h"
#include "app_version.h"            /* SAFE_VERSION_STRING（LWT 载荷） */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- 常量（全部静态定长，网线程内不申请堆内存） ---------------- */

#define QCAP                16      /* 两条队列的容量 */
#define TOPIC_MAX           64      /* 上行 topic */
#define PAYLOAD_MAX         512     /* 上行 payload */
#define MSG_TOPIC_MAX       128     /* 下行 topic */
#define MSG_PAYLOAD_MAX     512     /* 下行 payload */
#define RX_CAP              1024    /* 收包线性缓冲 */
#define TX_CAP              1024    /* 待发缓冲（部分写时的残留存放处） */
#define HOST_MAX            128
#define CID_MAX             64
#define CRED_MAX            64
#define WILL_TOPIC_MAX      64
#define WILL_PAYLOAD_MAX    160

/* mqtt_stop() 的 join 超时。★ 绝不用 pthread_cancel —— 那会让线程停在半写状态，
 * 套接字缓冲里留半个报文，对端收到一条残缺的 PUBLISH。超时就放弃 join。 */
#define MQTT_STOP_JOIN_MS   1000

/* 订阅的主题与 QoS（clean_session=1，每次建连都要重发 SUBSCRIBE） */
#define MQTT_SUB_TOPIC      "safe/cmd"
#define MQTT_SUB_QOS        1

/* ---------------- 上行指令队列（主线程 → net 线程，SPSC） ---------------- */

typedef struct {
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
    int  qos;
    int  retained;
} cmd_item_t;

/* ---------------- 下行消息队列（net 线程 → 主线程，SPSC） ---------------- */

typedef struct {
    char topic[MSG_TOPIC_MAX];
    char payload[MSG_PAYLOAD_MAX];
} msg_item_t;

/* ---------------- 统计（net 线程写、主线程读，用 volatile 镜像） ---------------- */

typedef struct {
    volatile uint32_t connects;
    volatile uint32_t reconnects;
    volatile uint32_t ping_timeout;
    volatile uint32_t retransmit;
    volatile uint32_t dropped;
    volatile uint32_t queue_full;
    volatile uint32_t qos0_fallback;
    volatile uint32_t rx_publish;
} mqtt_counters_t;

/* ---------------- 全局状态 ---------------- */

static char g_host[HOST_MAX];
static int  g_port = 1883;
static char g_client_id[CID_MAX];
static mqtt_msg_cb_t g_cb = NULL;

static char g_user[CRED_MAX];
static char g_pass[CRED_MAX];

static pthread_t        g_thread;
static volatile int     g_running = 0;
static volatile int     g_stop = 0;
static volatile int     g_thread_exited = 0;
static volatile int     g_conn = 0;          /* 已连上（CONNACK=0） */
static volatile int     g_state = 0;         /* mqtt_fsm_state_t 的镜像（供 mqtt_state_str） */

static int  g_wake_rd = -1;                  /* self-pipe 读端（进 poll） */
static int  g_wake_wr = -1;                  /* self-pipe 写端（任何线程唤醒 net 线程） */
static int  g_sock = -1;                     /* tls_stream_connect() 返回的描述符 */
static int  g_connecting = 0;                /* 非阻塞 connect 进行中 */

static mqtt_fsm_t       g_fsm;
static mqtt_inflight_t  g_inflight;
static uint8_t          g_tx[TX_CAP];
static size_t           g_tx_len = 0;
static size_t           g_tx_sent = 0;
static uint16_t         g_ctrl_id = 0;       /* SUBSCRIBE 等控制报文的 packet id（独立于 inflight） */

static cmd_item_t g_cmdq[QCAP];
static int        g_cmdq_head = 0, g_cmdq_tail = 0, g_cmdq_count = 0;
static pthread_mutex_t g_cmdq_m = PTHREAD_MUTEX_INITIALIZER;

static msg_item_t g_msgq[QCAP];
static int        g_msgq_head = 0, g_msgq_tail = 0, g_msgq_count = 0;
static pthread_mutex_t g_msgq_m = PTHREAD_MUTEX_INITIALIZER;

/* 最近一条下行 PUBLISH 的内容（DELIVER 时要用；v.payload 指向收包缓冲，
 * 随后的 memmove 会让它失效，所以必须在解析时拷出来） */
static struct {
    char     topic[MSG_TOPIC_MAX];
    char     payload[MSG_PAYLOAD_MAX];
    uint16_t pkt_id;
    uint8_t  qos;
} g_pending;

static mqtt_counters_t g_stat;

/* LWT */
static char g_will_topic[WILL_TOPIC_MAX];
static char g_will_payload[WILL_PAYLOAD_MAX];
static int  g_will_qos = 1;
static int  g_will_retain = 1;
static int  g_will_custom = 0;      /* 上层调过 mqtt_set_will 就不再用默认 LWT */
static int  g_will_logged = 0;      /* LWT 参数只打印一次，避免每次重连刷屏 */

/* ---------------- 小工具 ---------------- */

static void wake(void)
{
    if (g_wake_wr < 0) return;
    const char b = 0;
    ssize_t n = write(g_wake_wr, &b, 1);
    (void)n;   /* 管道满 / EAGAIN 都无所谓：字节无语义，只为打断 poll */
}

static void drain_wake(void)
{
    if (g_wake_rd < 0) return;
    char buf[64];
    for (int i = 0; i < 8; i++) {
        ssize_t n = read(g_wake_rd, buf, sizeof(buf));
        if (n <= 0) break;
    }
}

static uint16_t next_ctrl_id(void)
{
    if (g_ctrl_id >= 0xFFFF) g_ctrl_id = 0;
    g_ctrl_id++;
    if (g_ctrl_id == 0) g_ctrl_id = 1;
    return g_ctrl_id;
}

static void act_all(uint32_t now_ms, mqtt_fsm_out_t *out);

/* ---------------- 待发缓冲 ---------------- */

static bool tx_append(const uint8_t *buf, size_t len)
{
    if (len == 0 || len > TX_CAP) return false;
    if (g_tx_len + len > TX_CAP) return false;   /* 待发缓冲也满了：这条只能放弃 */
    memcpy(g_tx + g_tx_len, buf, len);
    g_tx_len += len;
    return true;
}

/* 返回：0 = 全部写完；1 = 写了部分；2 = 一个字节都没写（暂不可写）；-1 = 真错误 */
static int tx_flush(void)
{
    size_t before = g_tx_sent;
    while (g_tx_sent < g_tx_len) {
        int n = tls_stream_write(g_tx + g_tx_sent, g_tx_len - g_tx_sent);
        if (n > 0) { g_tx_sent += (size_t)n; continue; }
        if (n == 0) break;                      /* EAGAIN：等 POLLOUT */
        return -1;
    }
    if (g_tx_sent >= g_tx_len) { g_tx_len = 0; g_tx_sent = 0; return 0; }
    return (g_tx_sent > before) ? 1 : 2;
}

/* 组好包 → 入待发缓冲 → 尽力 flush → 把结果喂回状态机 */
static void tx_send(const uint8_t *buf, size_t len, uint32_t now_ms)
{
    mqtt_fsm_out_t o;
    if (!tx_append(buf, len)) {
        mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &o);
        act_all(now_ms, &o);
        return;
    }
    int r = tx_flush();
    if (r == 0 || r == 1) {
        /* 只要写出去了至少一个字节就算「本端有发送」，刷新 keepalive 判据 */
        mqtt_fsm_on_event(&g_fsm, MQTT_EV_TX_FLUSHED, NULL, now_ms, &o);
        act_all(now_ms, &o);
    } else if (r < 0) {
        mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &o);
        act_all(now_ms, &o);
    }
    /* r == 2：一个字节都没写（对端窗口满）→ 留着，等 POLLOUT 再 flush，不发 TX_FLUSHED */
}

/* ---------------- 动作执行 ---------------- */

static void do_close(void)
{
    tls_stream_close();
    g_sock = -1;
    g_connecting = 0;
    g_tx_len = 0;
    g_tx_sent = 0;
}

static void do_open(uint32_t now_ms)
{
    mqtt_fsm_out_t o;
    do_close();
    int fd = tls_stream_connect(g_host[0] ? g_host : "127.0.0.1",
                                g_port > 0 ? g_port : 1883, NULL);
    if (fd < 0) {
        g_sock = -1;
        /* 建连失败：立刻回灌状态机（否则要等「等 CONNACK」超时才退避，白白多等 20s） */
        mqtt_fsm_on_event(&g_fsm, MQTT_EV_SOCK_FAILED, NULL, now_ms, &o);
        act_all(now_ms, &o);
        return;
    }
    g_sock = fd;
    g_connecting = 1;
    g_tx_len = 0;
    g_tx_sent = 0;
}

static void build_will_payload(char *out, size_t cap)
{
    if (g_will_topic[0] == '\0') { out[0] = '\0'; return; }
    if (g_will_custom) {
        snprintf(out, cap, "%.127s", g_will_payload);
        return;
    }
    /* 默认 LWT：{"online":0,"ts":<now>,"fw":"..."}
     * （qos1 + retain 由 mqtt_pack_connect 固定连接标志位，见 mqtt_codec.c）。
     * 用 %.31s 限定版本串长度：定长缓冲 + snprintf 必须让编译器能证明不会截断，
     * 否则 -Wformat-truncation 会在 -Werror 下直接断构建。 */
    snprintf(out, cap, "{\"online\":0,\"ts\":%u,\"fw\":\"%.31s\"}",
             (unsigned)hal_time(), SAFE_VERSION_STRING);
}

static void do_send_connect(uint32_t now_ms)
{
    uint8_t buf[320];
    size_t n = 0;
    char will_msg[WILL_PAYLOAD_MAX];
    build_will_payload(will_msg, sizeof(will_msg));

    int r = mqtt_pack_connect(buf, sizeof(buf), &n,
                              g_client_id[0] ? g_client_id : "safe",
                              g_fsm.cfg.keepalive_s, true /* clean_session */,
                              g_will_topic[0] ? g_will_topic : NULL,
                              g_will_topic[0] ? will_msg : NULL,
                              g_user[0] ? g_user : NULL,
                              g_pass[0] ? g_pass : NULL);
    if (r != MQTT_OK) {
        mqtt_fsm_out_t o;
        mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &o);
        act_all(now_ms, &o);
        return;
    }
    /* LWT 的 QoS / Retain 由 mqtt_pack_connect 固定为 1 + true（见 mqtt_codec.c），
     * 上层经 mqtt_set_will() 传来的这两个值目前只作为意图记录下来 ——
     * 本客户端不需要别的组合，故未做成 codec 参数。这里打印一次便于核对。 */
    if (g_will_topic[0] != '\0' && !g_will_logged) {
        g_will_logged = 1;
        printf("[MQTT] will: %s qos=%d retain=%d\n",
               g_will_topic, g_will_qos, g_will_retain);
    }
    tx_send(buf, n, now_ms);
}

static void do_send_subscribe(uint32_t now_ms)
{
    uint8_t buf[128];
    size_t n = 0;
    uint16_t id = next_ctrl_id();
    int r = mqtt_pack_subscribe(buf, sizeof(buf), &n, MQTT_SUB_TOPIC, MQTT_SUB_QOS, id);
    if (r != MQTT_OK) return;
    tx_send(buf, n, now_ms);
}

static void do_send_pingreq(uint32_t now_ms)
{
    uint8_t buf[8];
    size_t n = 0;
    if (mqtt_pack_pingreq(buf, sizeof(buf), &n) != MQTT_OK) return;
    tx_send(buf, n, now_ms);
}

static void do_send_disconnect(uint32_t now_ms)
{
    uint8_t buf[8];
    size_t n = 0;
    if (mqtt_pack_disconnect(buf, sizeof(buf), &n) != MQTT_OK) return;
    tx_send(buf, n, now_ms);
}

static void do_send_puback(uint16_t id, uint32_t now_ms)
{
    uint8_t buf[8];
    size_t n = 0;
    if (mqtt_pack_puback(buf, sizeof(buf), &n, id) != MQTT_OK) return;
    tx_send(buf, n, now_ms);
}

/* 按 id 从 inflight 取回已组好的字节并发送（新发与重传走同一条路） */
static void do_send_inflight(uint16_t id, uint32_t now_ms)
{
    const mqtt_inflight_item_t *it = mqtt_inflight_find(&g_inflight, id);
    if (it == NULL || it->len == 0) return;
    tx_send(it->pkt, it->len, now_ms);
}

static void do_deliver(void)
{
    /* 上层注册了回调就走回调（沿用原实现的语义；注意回调在 **net 线程**执行，
     * rpc.c 传的是 NULL，实际不会走到这里）。 */
    if (g_cb != NULL) {
        g_cb(g_pending.topic, g_pending.payload);
        return;
    }
    int ok = 0;
    pthread_mutex_lock(&g_msgq_m);
    if (g_msgq_count < QCAP) {
        snprintf(g_msgq[g_msgq_head].topic, MSG_TOPIC_MAX, "%s", g_pending.topic);
        snprintf(g_msgq[g_msgq_head].payload, MSG_PAYLOAD_MAX, "%s", g_pending.payload);
        g_msgq_head = (g_msgq_head + 1) % QCAP;
        g_msgq_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_msgq_m);

    if (!ok) {
        /* ★ 背压取舍（A4 裁定）：入队失败 → **主动放弃 PUBACK**，让 broker 在重试
         * 超时后重投（DUP=1）。设备端代价只是处理变慢，但对端绝不会「以为成功了
         * 其实指令没了」。重投报文会命中 rpc_guard 的 req_id 去重表回 1004。 */
        mqtt_fsm_cancel_puback(&g_fsm);
        g_stat.queue_full++;
    }
}

/* NOTIFY_STATE：把状态与统计镜像给主线程读（避免主线程直接读状态机内部） */
static void do_notify(void)
{
    g_state = (int)g_fsm.state;
    g_stat.connects     = g_fsm.stat_connects;
    g_stat.reconnects   = g_fsm.stat_reconnects;
    g_stat.ping_timeout = g_fsm.stat_ping_timeout;
    g_stat.retransmit   = g_fsm.stat_retransmit;
    g_stat.dropped      = g_fsm.stat_dropped;
    g_stat.rx_publish   = g_fsm.stat_rx_publish;
    const int connected = (g_fsm.state == MQTT_ST_CONNECTED);
    if (g_conn != connected) {
        g_conn = connected;
        printf("[MQTT] %s (connack=%d)\n", connected ? "connected" : "disconnected",
               (int)g_fsm.connack_code);
    }
}

static void act_all(uint32_t now_ms, mqtt_fsm_out_t *out)
{
    while (out->act != MQTT_ACT_NONE) {
        mqtt_fsm_action_t a = out->act;
        uint16_t id = out->pkt_id;
        switch (a) {
        case MQTT_ACT_OPEN_SOCKET:      do_open(now_ms); break;
        case MQTT_ACT_CLOSE_SOCKET:     do_close(); break;
        case MQTT_ACT_SEND_CONNECT:     do_send_connect(now_ms); break;
        case MQTT_ACT_SEND_SUBSCRIBE:   do_send_subscribe(now_ms); break;
        case MQTT_ACT_SEND_PINGREQ:     do_send_pingreq(now_ms); break;
        case MQTT_ACT_SEND_DISCONNECT:  do_send_disconnect(now_ms); break;
        case MQTT_ACT_SEND_PUBLISH:     do_send_inflight(id, now_ms); break;
        case MQTT_ACT_SEND_PUBACK:      do_send_puback(id, now_ms); break;
        case MQTT_ACT_RETRANSMIT:       do_send_inflight(id, now_ms); break;
        case MQTT_ACT_DROP_INFLIGHT:    /* 已在状态机里记 stat_dropped，这里只提示 */
                                        printf("[MQTT] pkt %u 重传次数用尽，丢弃\n", (unsigned)id);
                                        break;
        case MQTT_ACT_DELIVER:          do_deliver(); break;
        case MQTT_ACT_NOTIFY_STATE:     do_notify(); break;
        case MQTT_ACT_NONE:
        default:                        break;
        }
        mqtt_fsm_tick(&g_fsm, now_ms, out);
    }
}

/* ---------------- 上行指令队列 ---------------- */

static int cmdq_push(const char *topic, const char *payload, int qos, int retained)
{
    int ok = 0;
    pthread_mutex_lock(&g_cmdq_m);
    if (g_cmdq_count >= QCAP) {
        /* 上行是事件 / 回执，**可丢**：权威记录是本地 safe.log，不是 MQTT 通道。
         * 丢最旧而不是拒新：状态上报总是最新的才有意义。 */
        g_cmdq_tail = (g_cmdq_tail + 1) % QCAP;
        g_cmdq_count--;
        g_stat.queue_full++;
    }
    snprintf(g_cmdq[g_cmdq_head].topic, TOPIC_MAX, "%s", topic ? topic : "");
    snprintf(g_cmdq[g_cmdq_head].payload, PAYLOAD_MAX, "%s", payload ? payload : "");
    g_cmdq[g_cmdq_head].qos      = qos;
    g_cmdq[g_cmdq_head].retained = retained;
    g_cmdq_head = (g_cmdq_head + 1) % QCAP;
    g_cmdq_count++;
    ok = 1;
    pthread_mutex_unlock(&g_cmdq_m);
    return ok;
}

static int cmdq_pop(cmd_item_t *out)
{
    int ok = 0;
    pthread_mutex_lock(&g_cmdq_m);
    if (g_cmdq_count > 0 && out != NULL) {
        *out = g_cmdq[g_cmdq_tail];
        g_cmdq_tail = (g_cmdq_tail + 1) % QCAP;
        g_cmdq_count--;
        ok = 1;
    }
    pthread_mutex_unlock(&g_cmdq_m);
    return ok;
}

static int cmdq_pending(void)
{
    pthread_mutex_lock(&g_cmdq_m);
    int n = g_cmdq_count;
    pthread_mutex_unlock(&g_cmdq_m);
    return n;
}

/* ★ QoS1 装不下（>512B）或 id 不一致时的降级通路：改为 QoS0 直发。
 * 安全事件的底线是「绝不静默丢」，宁可牺牲送达保证也要把字节发出去。 */
static void qos0_fallback(uint32_t now_ms, const cmd_item_t *it, mqtt_fsm_out_t *out)
{
    uint8_t pkt[PAYLOAD_MAX + TOPIC_MAX + 16];
    size_t n = 0;
    g_stat.qos0_fallback++;
    printf("[MQTT] 报文超长/未确认队列满，降级为 QoS0 直发（topic=%s）\n", it->topic);
    if (mqtt_pack_publish(pkt, sizeof(pkt), &n, it->topic,
                          (const uint8_t *)it->payload, strlen(it->payload),
                          0, false, 0, it->retained ? true : false) == MQTT_OK) {
        tx_send(pkt, n, now_ms);
    }
    out->act = MQTT_ACT_NONE;   /* tx_send 内部已消费动作 */
}

/* 「偷看」下一个将要被分配的 packet id。
 * 为什么必须偷看：QoS1 的 PUBLISH **字节里就带着** packet id，而 id 是在
 * mqtt_fsm_publish_begin() 内部调用 mqtt_inflight_next_id() 分配的 ——
 * 只有先知道它，才能组出一条「字节与 id 一致」的报文。
 * 这里的算式与 mqtt_inflight_next_id() 保持一致（先 ++、跳过 0、65535 回绕）；
 * 保险起见，调用方还会核对 publish_begin 实际返回的 id，不一致就拒绝发送。 */
static uint16_t peek_pkt_id(void)
{
    uint16_t n = g_inflight.next_id;
    if (n >= 0xFFFF) return 1;
    n = (uint16_t)(n + 1);
    if (n == 0) n = 1;
    return n;
}

/* 消费上行队列：持锁期间只做 memcpy，不做任何套接字 IO（规约：不持锁做重活） */
static void cmdq_drain(uint32_t now_ms, mqtt_fsm_out_t *out)
{
    /* 未连上时**不消费**队列：留着等重连后发（上行是事件 / 回执，晚一点没关系，
     * 权威记录在本地 safe.log）。若在这里强行发，套接字是 -1 → 立刻 IO_ERROR
     * → 状态机平白多一轮退避。队列满时由 cmdq_push 丢最旧。 */
    if (g_fsm.state != MQTT_ST_CONNECTED) return;

    cmd_item_t it;
    int budget = QCAP;
    while (budget-- > 0 && cmdq_pop(&it)) {
        uint8_t pkt[PAYLOAD_MAX + TOPIC_MAX + 16];
        size_t n = 0;
        const size_t plen = strlen(it.payload);
        const bool retain = it.retained ? true : false;

        if (it.qos == 1) {
            /* ★ QoS1 报文装不进 inflight（> MQTT_INFLIGHT_PKT_MAX = 512B）时
             * **不能静默丢** —— 安全事件丢不起。降级为 QoS0 直发并记一笔
             * stat_qos0_fallback（就 5 行，但不写就是静默丢安全事件）。 */
            uint16_t want = peek_pkt_id();
            if (mqtt_pack_publish(pkt, sizeof(pkt), &n, it.topic,
                                  (const uint8_t *)it.payload, plen,
                                  want, false, 1, retain) != MQTT_OK) {
                continue;
            }
            if (n > MQTT_INFLIGHT_PKT_MAX) {
                qos0_fallback(now_ms, &it, out);
                continue;
            }
            uint16_t got = mqtt_fsm_publish_begin(&g_fsm, pkt, n, now_ms, out);
            if (got != want) {
                /* 极少数情形下（如 id 冲突）分配到的 id 与组包用的不一致：
                 * 绝不能发出一条「字节里的 id 与队列键不一致」的报文 ——
                 * PUBACK 会对不上号。撤掉刚才的入队，降级 QoS0 直发。 */
                if (got != 0) (void)mqtt_inflight_ack(&g_inflight, got);
                qos0_fallback(now_ms, &it, out);
                continue;
            }
            act_all(now_ms, out);
        } else {
            if (mqtt_pack_publish(pkt, sizeof(pkt), &n, it.topic,
                                  (const uint8_t *)it.payload, plen,
                                  0, false, 0, retain) != MQTT_OK) {
                continue;
            }
            tx_send(pkt, n, now_ms);
            out->act = MQTT_ACT_NONE;   /* tx_send 内部已消费掉动作，避免外层重复执行 */
        }
    }
}

/* ---------------- 收包 ---------------- */

static void capture_pending(const mqtt_pkt_view_t *v)
{
    const uint8_t *pl = NULL;
    size_t pln = 0;
    uint16_t pid = 0;
    bool dup = false, ret = false;
    uint8_t qos = 0;
    memset(&g_pending, 0, sizeof(g_pending));
    if (mqtt_parse_publish(v, g_pending.topic, sizeof(g_pending.topic),
                           &pl, &pln, &pid, &dup, &qos, &ret) != MQTT_OK) return;
    size_t n = pln;
    if (n > MSG_PAYLOAD_MAX - 1) n = MSG_PAYLOAD_MAX - 1;
    if (pl != NULL && n > 0) memcpy(g_pending.payload, pl, n);
    g_pending.payload[n] = '\0';
    g_pending.pkt_id = pid;
    g_pending.qos    = qos;
}

/* ---------------- net 线程 ---------------- */

static void * mqtt_net_thread(void *arg)
{
    uint8_t rx[RX_CAP];
    size_t  rx_len = 0;
    mqtt_fsm_out_t out;
    uint32_t now_ms;

    (void)arg;

    mqtt_fsm_cfg_t cfg;
    mqtt_fsm_cfg_default(&cfg);
    mqtt_fsm_init(&g_fsm, &cfg, &g_inflight);
    mqtt_inflight_init(&g_inflight);

    now_ms = hal_time_ms();
    mqtt_fsm_on_event(&g_fsm, MQTT_EV_START, NULL, now_ms, &out);
    act_all(now_ms, &out);

    while (!g_stop) {
        now_ms = hal_time_ms();

        /* 1) 排空 self-pipe（唤醒字节无语义，只用来打断 poll） */
        drain_wake();

        /* 2) 消费上行指令队列（持锁只做 memcpy，不做套接字 IO） */
        cmdq_drain(now_ms, &out);

        /* 3) 驱动状态机：tick + 把产出的动作全部执行掉 */
        mqtt_fsm_tick(&g_fsm, now_ms, &out);
        act_all(now_ms, &out);

        /* 4) poll 超时 = min(状态机定时器；有上行指令 / 有待发残留则 0) */
        int timeout = (int)mqtt_fsm_next_wake_ms(&g_fsm, now_ms);
        if (cmdq_pending() > 0) timeout = 0;
        if (g_tx_len > g_tx_sent) timeout = 0;
        if (timeout < 0) timeout = 0;

        /* 5) poll 双 fd：套接字 + self-pipe */
        struct pollfd fds[2];
        fds[0].fd = -1; fds[0].events = 0; fds[0].revents = 0;
        fds[1].fd = g_wake_rd; fds[1].events = POLLIN; fds[1].revents = 0;
        if (g_sock >= 0) {
            fds[0].fd = g_sock;
            fds[0].events = POLLIN;
            if (g_connecting) fds[0].events |= POLLOUT;
            if (g_tx_len > g_tx_sent) fds[0].events |= POLLOUT;
        }
        int n = poll(fds, 2, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &out);
            act_all(now_ms, &out);
            continue;
        }

        /* 6) 非阻塞 connect 进度 */
        if (g_connecting && g_sock >= 0 && (fds[0].revents & POLLOUT)) {
            int r = tls_stream_connect_poll(0);
            g_connecting = 0;
            if (r == 0) {
                mqtt_fsm_on_event(&g_fsm, MQTT_EV_SOCK_CONNECTED, NULL, now_ms, &out);
            } else if (r < 0) {
                mqtt_fsm_on_event(&g_fsm, MQTT_EV_SOCK_FAILED, NULL, now_ms, &out);
            }
            act_all(now_ms, &out);
            if (r < 0) continue;
        }

        /* 7) 可写：flush 待发残留 */
        if (g_sock >= 0 && (fds[0].revents & POLLOUT) && g_tx_len > g_tx_sent) {
            int r = tx_flush();
            if (r == 0 || r == 1) {
                mqtt_fsm_on_event(&g_fsm, MQTT_EV_TX_FLUSHED, NULL, now_ms, &out);
                act_all(now_ms, &out);
            } else if (r < 0) {
                mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &out);
                act_all(now_ms, &out);
                continue;
            }
        }

        /* 8) 可读：尽量读满，循环解析（粘包 / 半包） */
        if (g_sock >= 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            int io_err = 0;
            for (;;) {
                if (rx_len >= sizeof(rx)) break;
                int m = tls_stream_read(rx + rx_len, sizeof(rx) - rx_len, 0);
                if (m > 0) { rx_len += (size_t)m; continue; }
                if (m == 0) io_err = 1;        /* EOF：对端关闭 */
                else if (m < -1) io_err = 1;   /* 真错误 */
                break;                          /* m == -1 = 暂无数据，正常 */
            }

            size_t off = 0;
            while (off < rx_len) {
                mqtt_pkt_view_t v;
                size_t used = 0;
                int pr = mqtt_parse(rx + off, rx_len - off, &v, &used);
                if (pr == MQTT_ERR_TRUNCATED) break;          /* 半包：继续收 */
                if (pr != MQTT_OK) { io_err = 1; break; }     /* 协议错：不可自愈 */
                if (v.type == MQTT_PKT_PUBLISH) capture_pending(&v);
                (void)mqtt_fsm_on_packet(&g_fsm, &v, now_ms, &out);
                act_all(now_ms, &out);
                off += used;
            }
            if (off > 0) {
                memmove(rx, rx + off, rx_len - off);
                rx_len -= off;
            }
            if (io_err) {
                mqtt_fsm_on_event(&g_fsm, MQTT_EV_IO_ERROR, NULL, now_ms, &out);
                act_all(now_ms, &out);
            }
        }
    }

    /* 退出前关链路（管道由 mqtt_stop 决定是否回收） */
    do_close();
    g_thread_exited = 1;
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

int mqtt_start(const char *host, int port, const char *client_id, mqtt_msg_cb_t cb)
{
    if (g_running) return 0;

    g_cb = cb;
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "127.0.0.1");
    g_port = (port > 0) ? port : 1883;
    snprintf(g_client_id, sizeof(g_client_id), "%s",
             (client_id && client_id[0]) ? client_id : "safe");
    memset(&g_stat, 0, sizeof(g_stat));
    g_stop = 0;
    g_thread_exited = 0;
    g_conn = 0;
    g_state = (int)MQTT_ST_IDLE;
    g_tx_len = g_tx_sent = 0;
    g_ctrl_id = 0;
    g_cmdq_head = g_cmdq_tail = g_cmdq_count = 0;
    g_msgq_head = g_msgq_tail = g_msgq_count = 0;

    /* 默认 LWT（上层调过 mqtt_set_will 就不覆盖） */
    if (!g_will_custom && g_will_topic[0] == '\0') {
        snprintf(g_will_topic, sizeof(g_will_topic), "safe/status");
        g_will_qos = 1;
        g_will_retain = 1;
    }

    /* self-pipe：为什么不用条件变量 —— 线程要**同时**等「套接字 IO」和「上层指令」，
     * condvar 无法与 fd 一起等待。self-pipe 是 Unix 下把任意事件统一进 poll 的标准手段。
     * 为什么不用 eventfd —— eventfd 是 Linux 专有，本项目要保 PC / 板子双构建。 */
    int p[2];
    if (pipe(p) != 0) return -1;
    g_wake_rd = p[0];
    g_wake_wr = p[1];
    (void)fcntl(g_wake_rd, F_SETFL, O_NONBLOCK);
    (void)fcntl(g_wake_wr, F_SETFL, O_NONBLOCK);

    if (pthread_create(&g_thread, NULL, mqtt_net_thread, NULL) != 0) {
        close(g_wake_rd);
        close(g_wake_wr);
        g_wake_rd = g_wake_wr = -1;
        return -1;
    }
    g_running = 1;
    return 0;
}

void mqtt_stop(void)
{
    if (!g_running) return;
    g_stop = 1;
    wake();

    /* ★ join 必须带超时：net 线程可能在等 PUBACK（inflight 非空）或正阻塞在
     * 退避 poll 里。超时就放弃 join，**绝不 pthread_cancel** ——
     * cancel 会让线程停在半写状态，套接字缓冲里留半个报文，得不偿失。
     * 每轮都再写一次 self-pipe，确保它不会睡在长退避里错过 g_stop。 */
    uint32_t t0 = hal_time_ms();
    int joined = 0;
    while (!g_thread_exited) {
        if ((uint32_t)(hal_time_ms() - t0) >= (uint32_t)MQTT_STOP_JOIN_MS) break;
        wake();
        usleep(5000);
    }
    if (g_thread_exited) { pthread_join(g_thread, NULL); joined = 1; }
    else                 { pthread_detach(g_thread); }

    /* 优雅退出已发 DISCONNECT → broker **不发** LWT（只有异常断开才发），
     * 这是 T05 验收里最容易误判的一条。 */
    do_close();
    if (joined) {
        if (g_wake_rd >= 0) close(g_wake_rd);
        if (g_wake_wr >= 0) close(g_wake_wr);
        g_wake_rd = g_wake_wr = -1;
    }
    g_running = 0;
    g_conn = 0;
    g_state = (int)MQTT_ST_IDLE;
}

int mqtt_publish(const char *topic, const char *payload, int qos, int retained)
{
    if (!g_running) return -1;
    if (topic == NULL || payload == NULL) return -1;
    /* 队列满时丢最旧（上行是事件 / 回执，权威记录在 safe.log），不阻塞主线程。 */
    (void)cmdq_push(topic, payload, qos, retained);
    wake();                 /* 写 self-pipe 唤醒 net 线程，别让它睡在长 poll 里 */
    return 0;
}

bool mqtt_is_connected(void) { return g_conn != 0; }

void mqtt_set_credentials(const char *username, const char *password)
{
    snprintf(g_user, sizeof(g_user), "%s", (username && *username) ? username : "");
    snprintf(g_pass, sizeof(g_pass), "%s", (password && *password) ? password : "");
    if ((g_user[0] != '\0') != (g_pass[0] != '\0')) {
        printf("[MQTT] 凭据不完整（仅设置了%s），按「已配置凭据」处理\n",
               (g_user[0] != '\0') ? "用户名" : "密码");
    }
}

bool mqtt_credentials_configured(void)
{
    return (g_user[0] != '\0') || (g_pass[0] != '\0');
}

bool mqtt_is_authenticated(void)
{
    /* 已配置凭据且已连上 broker 即视为鉴权连接。
     * （无 TLS 时客户端无法反向验证 broker 是否真的校验了凭据，此为尽最大努力门控。） */
    return (g_conn != 0) && mqtt_credentials_configured();
}

/* 主线程取走一条消息（rpc_poll 调用）。返回 1 表示取到。 */
int mqtt_take(char *topic, size_t tcap, char *payload, size_t pcap)
{
    if (topic == NULL || payload == NULL || tcap == 0 || pcap == 0) return 0;
    int got = 0;
    pthread_mutex_lock(&g_msgq_m);
    if (g_msgq_count > 0) {
        snprintf(topic, tcap, "%s", g_msgq[g_msgq_tail].topic);
        snprintf(payload, pcap, "%s", g_msgq[g_msgq_tail].payload);
        g_msgq_tail = (g_msgq_tail + 1) % QCAP;
        g_msgq_count--;
        got = 1;
    }
    pthread_mutex_unlock(&g_msgq_m);
    return got;
}

/* ---- R9 T04 新增（规约 §5.10.1 已预留；mqtt_stub.c 必须同步实现） ---- */

void mqtt_set_will(const char *topic, const char *payload, int qos, bool retain)
{
    if (topic == NULL || topic[0] == '\0') { g_will_topic[0] = '\0'; return; }
    snprintf(g_will_topic, sizeof(g_will_topic), "%s", topic);
    snprintf(g_will_payload, sizeof(g_will_payload), "%s", payload ? payload : "");
    g_will_qos = (qos == 1) ? 1 : 0;
    g_will_retain = retain ? 1 : 0;
    g_will_custom = 1;
}

const char * mqtt_state_str(void)
{
    return mqtt_fsm_state_name((mqtt_fsm_state_t)g_state);
}

void mqtt_stats(uint32_t *connects, uint32_t *reconnects, uint32_t *dropped, uint32_t *queue_full)
{
    if (connects)    *connects    = (uint32_t)g_stat.connects;
    if (reconnects)  *reconnects  = (uint32_t)g_stat.reconnects;
    if (dropped)     *dropped     = (uint32_t)g_stat.dropped;
    if (queue_full)  *queue_full  = (uint32_t)g_stat.queue_full;
}

size_t mqtt_queued(void)
{
    size_t n = 0;
    pthread_mutex_lock(&g_msgq_m);
    n = (size_t)g_msgq_count;
    pthread_mutex_unlock(&g_msgq_m);
    return n;
}
