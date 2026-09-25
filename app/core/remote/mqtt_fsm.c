/**
 * @file mqtt_fsm.c
 * R9 T03：MQTT 连接状态机实现（纯逻辑）。
 *
 * 本文件的三条铁律（R9 验收判据会机械检查，勿破）：
 *   1. 不建套接字、不起线程 —— 只由 T04 的 net 线程驱动；
 *   2. 不取时间（HAL 时钟也不取）—— 时刻一律由 now_ms 参数注入；
 *   3. 不申请堆内存、不打印 —— 全部静态/栈上，抖动随机数用自带 xorshift32。
 *
 * 抖动为什么不用 rand()：rand() 是进程全局状态，glibc 下带内部锁且跨 libc 实现
 * 不同 → 单测结果不可确定、不可移植。自带 12 行 xorshift32、种子从 cfg.rng_seed
 * 注入，单测就能断言**精确的重连时刻序列**。这是「可测性驱动实现选择」的实例。
 */
#include "core/remote/mqtt_fsm.h"

#include <string.h>

/* 一次事件最多产出的动作数。当前上限是 2（如 CONNACK(0) → SUBSCRIBE + NOTIFY），
 * 给 4 留余量，避免将来加动作时静默丢。 */
#define FSM_ACT_CAP 4

/* on_packet 解析 PUBLISH 时的 topic 暂存（栈上，不申请堆内存） */
#define FSM_TOPIC_CAP 128

/* DISCONNECTING 的兜底等待：正常路径由 MQTT_EV_TX_FLUSHED 立刻收敛到 IDLE；
 * 这条只防「写了 DISCONNECT 却再也等不到 TX_FLUSHED」时状态机永远悬着。 */
#define FSM_DISCONNECT_WAIT_MS 1000u

/* IDLE 时的唤醒间隔：让调用方周期性醒一次，便于响应 START。 */
#define FSM_IDLE_WAKE_MS 1000u

/* 各配置项的兜底值（cfg 传 0 时不会除零/永不超时） */
#define FSM_DEF_KEEPALIVE_MS     20000u
#define FSM_DEF_PING_TIMEOUT_MS  20000u
#define FSM_DEF_RETRY_TIMEOUT_MS  5000u
#define FSM_DEF_BACKOFF_BASE_MS    500u
#define FSM_DEF_BACKOFF_MAX_MS   30000u

/* ---------------- 内部：动作 FIFO ---------------- */

static void act_push(mqtt_fsm_t *f, mqtt_fsm_action_t a, uint16_t pkt_id)
{
    if (f == NULL || f->act_n >= FSM_ACT_CAP) return;
    uint8_t slot = (uint8_t)((f->act_head + f->act_n) % FSM_ACT_CAP);
    f->acts[slot].act           = a;
    f->acts[slot].pkt_id        = pkt_id;
    f->acts[slot].next_wake_ms  = 0;
    f->acts[slot].connack_code  = (int)f->connack_code;
    f->acts[slot].state_changed = (a == MQTT_ACT_NOTIFY_STATE);
    f->act_n++;
}

static int act_pop(mqtt_fsm_t *f, mqtt_fsm_out_t *out)
{
    if (f == NULL || out == NULL || f->act_n == 0) return 0;
    *out = f->acts[f->act_head];
    f->act_head = (uint8_t)((f->act_head + 1) % FSM_ACT_CAP);
    f->act_n--;
    return 1;
}

/* 无动作时填一个「空」输出，但把 next_wake_ms 算好给调用方当 poll 超时。 */
static void act_none(mqtt_fsm_t *f, uint32_t now_ms, mqtt_fsm_out_t *out)
{
    if (out == NULL) return;
    if (f == NULL) { memset(out, 0, sizeof(*out)); return; }
    out->act           = MQTT_ACT_NONE;
    out->pkt_id        = 0;
    out->connack_code  = (int)f->connack_code;
    out->state_changed = false;
    out->next_wake_ms  = mqtt_fsm_next_wake_ms(f, now_ms);
}

/* ---------------- 内部：抖动 PRNG（xorshift32） ---------------- */

static uint32_t fsm_rand(mqtt_fsm_t *f)
{
    uint32_t x = f->rng;
    /* 种子为 0 时 xorshift 恒为 0（退化），兜底换一个非零常量，
     * 保证「cfg.rng_seed = 0」这种写法不会得到一条全零的退避序列。 */
    if (x == 0u) x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    f->rng = x;
    return x;
}

/* ---------------- 内部：退避 ---------------- */

static uint32_t fsm_backoff_next(mqtt_fsm_t *f)
{
    uint32_t base = f->cfg.backoff_base_ms ? f->cfg.backoff_base_ms : FSM_DEF_BACKOFF_BASE_MS;
    uint32_t max  = f->cfg.backoff_max_ms  ? f->cfg.backoff_max_ms  : FSM_DEF_BACKOFF_MAX_MS;
    uint8_t  sh   = (f->retry_count > 5) ? (uint8_t)5 : f->retry_count;

    uint32_t b = base << sh;                    /* 指数增长，最多 <<5（32 倍） */
    if (b == 0u || b > max) b = max;            /* 溢出或超封顶 → 钉在封顶值 */

    /* 抖动：delay ∈ [0.75b, b]。
     * 目的：避免同一 broker 上一批设备同时重连形成同步风暴（惊群）。 */
    uint32_t q = b / 4u;
    return (b - q) + (fsm_rand(f) % (q + 1u));
}

/* 进入 WAIT_RETRY：算退避、置到期时刻、清与「已建连」相关的标志。 */
static void fsm_enter_retry(mqtt_fsm_t *f, uint32_t now_ms)
{
    uint32_t d = fsm_backoff_next(f);
    f->backoff_ms     = d;
    f->retry_at_ms    = now_ms + d;
    if (f->retry_count < 0xFFu) f->retry_count++;
    f->state          = MQTT_ST_WAIT_RETRY;
    f->pending_ping   = false;
    f->ping_sent_ms   = 0;
    f->pending_puback = 0;
}

/* ---------------- 对外接口 ---------------- */

void mqtt_fsm_cfg_default(mqtt_fsm_cfg_t *cfg)
{
    if (cfg == NULL) return;
    cfg->keepalive_s      = 20;
    cfg->ping_timeout_ms  = 20000u;   /* = keepalive*1000：一个 keepalive 周期收不到 PINGRESP 即判半开 */
    cfg->retry_timeout_ms = 5000u;
    cfg->backoff_base_ms  = 500u;
    /* 封顶 30s（而非原第三方库默认的 8s）：broker 重启场景下 8s 封顶会在 30s 内产生
     * ~6 次注定失败的重连，每次都要走域名解析/TCP/建连，对单核 A7 是纯浪费。
     * 代价是故障恢复感知慢，但有 LWT + 5s 状态快照兜底，可接受。 */
    cfg->backoff_max_ms   = 30000u;
    cfg->rng_seed         = 0x2545F491u;
}

void mqtt_fsm_init(mqtt_fsm_t *f, const mqtt_fsm_cfg_t *cfg, mqtt_inflight_t *q)
{
    if (f == NULL) return;
    memset(f, 0, sizeof(*f));
    if (cfg != NULL) f->cfg = *cfg;
    else             mqtt_fsm_cfg_default(&f->cfg);
    f->rng = f->cfg.rng_seed;
    /* clean_session=1：每次建连都要重新 SUBSCRIBE，故初始即置位。 */
    f->pending_subscribe = true;
    f->inflight = q;
    f->state = MQTT_ST_IDLE;
}

const char * mqtt_fsm_state_name(mqtt_fsm_state_t s)
{
    switch (s) {
    case MQTT_ST_IDLE:          return "idle";
    case MQTT_ST_CONNECTING:    return "connecting";
    case MQTT_ST_CONNECTED:     return "connected";
    case MQTT_ST_WAIT_RETRY:    return "retry";
    case MQTT_ST_DISCONNECTING: return "disconnecting";
    }
    return "unknown";
}

const char * mqtt_fsm_action_name(mqtt_fsm_action_t a)
{
    switch (a) {
    case MQTT_ACT_NONE:            return "NONE";
    case MQTT_ACT_OPEN_SOCKET:     return "OPEN_SOCKET";
    case MQTT_ACT_CLOSE_SOCKET:    return "CLOSE_SOCKET";
    case MQTT_ACT_SEND_CONNECT:    return "SEND_CONNECT";
    case MQTT_ACT_SEND_SUBSCRIBE:  return "SEND_SUBSCRIBE";
    case MQTT_ACT_SEND_PINGREQ:    return "SEND_PINGREQ";
    case MQTT_ACT_SEND_DISCONNECT: return "SEND_DISCONNECT";
    case MQTT_ACT_SEND_PUBLISH:    return "SEND_PUBLISH";
    case MQTT_ACT_SEND_PUBACK:     return "SEND_PUBACK";
    case MQTT_ACT_RETRANSMIT:      return "RETRANSMIT";
    case MQTT_ACT_DROP_INFLIGHT:   return "DROP_INFLIGHT";
    case MQTT_ACT_DELIVER:         return "DELIVER";
    case MQTT_ACT_NOTIFY_STATE:    return "NOTIFY_STATE";
    }
    return "UNKNOWN";
}

uint32_t mqtt_fsm_next_wake_ms(const mqtt_fsm_t *f, uint32_t now_ms)
{
    if (f == NULL) return 0;
    /* 还有没消费完的动作、或有一条待发的 PUBACK → 立刻动，不要等。 */
    if (f->act_n > 0 || f->pending_puback != 0) return 0u;

    switch (f->state) {
    case MQTT_ST_IDLE:
        return FSM_IDLE_WAKE_MS;

    case MQTT_ST_CONNECTING: {
        /* 等 CONNACK 的超时：没有它，一个被黑洞吞掉的 SYN 会让状态机永远悬在
         * CONNECTING（设计文档 §2.7 的定时器表未列此项，属实现侧必要补充）。 */
        uint32_t t = f->cfg.ping_timeout_ms ? f->cfg.ping_timeout_ms : FSM_DEF_PING_TIMEOUT_MS;
        uint32_t e = now_ms - f->last_tx_ms;
        return (e >= t) ? 0u : (t - e);
    }

    case MQTT_ST_DISCONNECTING: {
        uint32_t e = now_ms - f->last_tx_ms;
        return (e >= FSM_DISCONNECT_WAIT_MS) ? 0u : (FSM_DISCONNECT_WAIT_MS - e);
    }

    case MQTT_ST_WAIT_RETRY: {
        if (f->retry_at_ms == 0u) return 0u;
        if ((int32_t)(now_ms - f->retry_at_ms) >= 0) return 0u;
        return (uint32_t)(f->retry_at_ms - now_ms);
    }

    case MQTT_ST_CONNECTED: {
        uint32_t ka = ((uint32_t)f->cfg.keepalive_s) * 1000u;
        if (ka == 0u) ka = FSM_DEF_KEEPALIVE_MS;
        uint32_t e = now_ms - f->last_tx_ms;
        uint32_t best = (e >= ka) ? 0u : (ka - e);

        if (f->pending_ping) {
            uint32_t pt = f->cfg.ping_timeout_ms ? f->cfg.ping_timeout_ms : ka;
            uint32_t ep = now_ms - f->ping_sent_ms;
            uint32_t rp = (ep >= pt) ? 0u : (pt - ep);
            if (rp < best) best = rp;
        }

        if (f->inflight != NULL) {
            uint32_t rt = f->cfg.retry_timeout_ms ? f->cfg.retry_timeout_ms : FSM_DEF_RETRY_TIMEOUT_MS;
            for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
                if (!f->inflight->items[i].in_use) continue;
                uint32_t ei = now_ms - f->inflight->items[i].sent_at_ms;
                uint32_t ri = (ei >= rt) ? 0u : (rt - ei);
                if (ri < best) best = ri;
            }
        }
        return best;
    }
    }
    return FSM_IDLE_WAKE_MS;
}

void mqtt_fsm_cancel_puback(mqtt_fsm_t *f)
{
    if (f == NULL) return;
    f->pending_puback = 0;
    /* 同步把 FIFO 里可能已排队的 SEND_PUBACK 摘掉并**紧凑前移**，
     * 保证状态与待执行动作一致（不能只置 NONE，那会让调用方的
     * `while (act != NONE)` 提前退出、把后面的动作留在队列里）。 */
    if (f->act_n > 0) {
        mqtt_fsm_out_t keep[FSM_ACT_CAP];
        uint8_t kn = 0;
        for (uint8_t k = 0; k < f->act_n; k++) {
            mqtt_fsm_out_t a = f->acts[(uint8_t)((f->act_head + k) % FSM_ACT_CAP)];
            if (a.act == MQTT_ACT_SEND_PUBACK) continue;
            keep[kn++] = a;
        }
        for (uint8_t k = 0; k < kn; k++) f->acts[k] = keep[k];
        f->act_head = 0;
        f->act_n    = kn;
    }
}

void mqtt_fsm_tick(mqtt_fsm_t *f, uint32_t now_ms, mqtt_fsm_out_t *out)
{
    if (f == NULL) {
        if (out != NULL) memset(out, 0, sizeof(*out));
        return;
    }

    /* 1) 先把上一次事件产出的、还没消费完的动作吐出来 */
    if (act_pop(f, out)) {
        if (out != NULL) out->next_wake_ms = mqtt_fsm_next_wake_ms(f, now_ms);
        return;
    }

    switch (f->state) {
    case MQTT_ST_IDLE:
        break;

    case MQTT_ST_CONNECTING: {
        uint32_t t = f->cfg.ping_timeout_ms ? f->cfg.ping_timeout_ms : FSM_DEF_PING_TIMEOUT_MS;
        if ((uint32_t)(now_ms - f->last_tx_ms) >= t) {
            /* 迟迟收不到 CONNACK：按连接失败处理 → 关链路 + 退避重连 */
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            fsm_enter_retry(f, now_ms);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;
    }

    case MQTT_ST_DISCONNECTING:
        if ((uint32_t)(now_ms - f->last_tx_ms) >= FSM_DISCONNECT_WAIT_MS) {
            f->state = MQTT_ST_IDLE;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_ST_WAIT_RETRY:
        if (f->retry_at_ms != 0u && (int32_t)(now_ms - f->retry_at_ms) >= 0) {
            f->retry_at_ms = 0;
            f->state       = MQTT_ST_CONNECTING;
            f->last_tx_ms  = now_ms;
            /* ★ 重连必须重新订阅（修「重连后永久失聪」这个 P0 缺陷）。
             *
             * 缺陷原状：pending_subscribe 只在 mqtt_fsm_init() 与 MQTT_EV_START
             * 置 true，而 MQTT_EV_START **全进程只发一次**（net 线程启动时）。
             * CONNACK 后发完 SUBSCRIBE 就被置 false，于是**此后的每一次重连
             * 都不会再发 SUBSCRIBE**。
             *
             * 为什么改这一处就够了（已逐一核对）：全文件只有两处把 state 置成
             * MQTT_ST_CONNECTING —— 本处（WAIT_RETRY 退避到期）与 MQTT_EV_START
             * （后者已置位）。而所有断链路径（SOCK_FAILED / IO_ERROR / CONNACK
             * 超时 / PING 超时）都先收敛到 WAIT_RETRY 再回到这里，所以本处是
             * **唯一的重连入口**，在这里置位即覆盖全部重连场景。
             *
             * 为什么它是致命的：本项目 clean_session=1（:390 的注释就是这么写的），
             * broker 在连接断开时会**丢掉订阅关系**。所以重连后设备虽然
             * CONNACK 成功、状态显示「已连接」、safe/status 照常上行，
             * 但**从此再也收不到任何下行指令** —— 对保险柜来说就是
             *「界面显示在线，远程开锁却永远不响应」，而且没有任何报错。
             * 触发条件极低：一次网络抖动 / broker 重启 / PING 超时都够。
             *
             * 判据（已实测复现，见提交说明）：连上 → 注入一条 >1024B 的下行报文
             * 触发设备侧断链重连 → 重连后再下发 query_status →
             *   修复前：safe/log **没有任何回执**（但 safe/status 上行照常，
             *           界面显示"在线"，且零报错）；
             *   修复后：safe/log 出现回执。 */
            f->pending_subscribe = true;
            act_push(f, MQTT_ACT_OPEN_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_ST_CONNECTED:
        /* (a) 下行 QoS1 的 PUBACK 优先：越早回，broker 越不容易重投 */
        if (f->pending_puback != 0) {
            act_push(f, MQTT_ACT_SEND_PUBACK, f->pending_puback);
            f->pending_puback = 0;
            break;
        }
        /* (b) PING 超时 → 半开连接：关链路 + 退避重连 */
        if (f->pending_ping) {
            uint32_t ka = ((uint32_t)f->cfg.keepalive_s) * 1000u;
            if (ka == 0u) ka = FSM_DEF_KEEPALIVE_MS;
            uint32_t pt = f->cfg.ping_timeout_ms ? f->cfg.ping_timeout_ms : ka;
            if ((uint32_t)(now_ms - f->ping_sent_ms) >= pt) {
                f->stat_ping_timeout++;
                act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
                fsm_enter_retry(f, now_ms);
                act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
                break;
            }
        }
        /* (c) keepalive：只记**发送**（规范约束的是本端发送间隔，收到 broker
         *     报文不刷新 last_tx_ms —— 这是最容易写错的一条） */
        if (!f->pending_ping) {
            uint32_t ka = ((uint32_t)f->cfg.keepalive_s) * 1000u;
            if (ka == 0u) ka = FSM_DEF_KEEPALIVE_MS;
            if ((uint32_t)(now_ms - f->last_tx_ms) >= ka) {
                f->pending_ping = true;
                f->ping_sent_ms = now_ms;
                f->last_tx_ms   = now_ms;
                act_push(f, MQTT_ACT_SEND_PINGREQ, 0);
                break;
            }
        }
        /* (d) QoS1 重传 */
        if (f->inflight != NULL) {
            uint32_t rt = f->cfg.retry_timeout_ms ? f->cfg.retry_timeout_ms : FSM_DEF_RETRY_TIMEOUT_MS;
            uint16_t id = 0;
            const uint8_t *pkt = NULL;
            size_t len = 0;
            if (mqtt_inflight_timeout_scan(f->inflight, now_ms, rt, &id, &pkt, &len) == MQTT_OK) {
                (void)pkt; (void)len;   /* 字节由调用方在 mark_sent 前自行取用 */
                /* ★ 这里在检出超时的同一拍就 mark_sent：刷新 sent_at 并累加 retry，
                 *   次数用尽则该条被丢弃、返回 MALFORMED → 转 DROP_INFLIGHT。
                 *   取舍：严格做法是「真正写出去之后再 mark_sent」，那需要调用方
                 *   把写结果回灌状态机（多一个事件）。真实链路上「检出超时」与
                 *   「写出去」只差几十微秒，写失败的最坏后果只是重传计数多加一次，
                 *   换来的是状态机无回灌、单测完全确定 —— 值。 */
                if (mqtt_inflight_mark_sent(f->inflight, id, now_ms) == MQTT_OK) {
                    f->stat_retransmit++;
                    act_push(f, MQTT_ACT_RETRANSMIT, id);
                } else {
                    f->stat_dropped++;
                    act_push(f, MQTT_ACT_DROP_INFLIGHT, id);
                }
            }
        }
        break;
    }

    if (act_pop(f, out)) {
        if (out != NULL) out->next_wake_ms = mqtt_fsm_next_wake_ms(f, now_ms);
        return;
    }
    act_none(f, now_ms, out);
}

void mqtt_fsm_on_event(mqtt_fsm_t *f, mqtt_fsm_ev_t ev, const void *arg,
                       uint32_t now_ms, mqtt_fsm_out_t *out)
{
    if (f == NULL) {
        if (out != NULL) memset(out, 0, sizeof(*out));
        return;
    }

    switch (ev) {
    case MQTT_EV_START:
        if (f->state == MQTT_ST_IDLE || f->state == MQTT_ST_WAIT_RETRY) {
            f->state             = MQTT_ST_CONNECTING;
            f->retry_count       = 0;
            f->backoff_ms        = 0;
            f->retry_at_ms       = 0;
            f->pending_ping      = false;
            f->ping_sent_ms      = 0;
            f->pending_puback    = 0;
            f->connack_code      = 0;
            f->session_present   = false;
            f->pending_subscribe = true;   /* clean_session=1：每次建连都重发 SUBSCRIBE */
            f->last_tx_ms        = now_ms;
            act_push(f, MQTT_ACT_OPEN_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_EV_STOP:
        f->pending_puback = 0;
        f->pending_ping   = false;
        switch (f->state) {
        case MQTT_ST_CONNECTED:
            f->state      = MQTT_ST_DISCONNECTING;
            f->last_tx_ms = now_ms;
            act_push(f, MQTT_ACT_SEND_DISCONNECT, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
            break;
        case MQTT_ST_CONNECTING:
            f->state      = MQTT_ST_IDLE;
            f->retry_at_ms = 0;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
            break;
        case MQTT_ST_WAIT_RETRY:
            f->state      = MQTT_ST_IDLE;
            f->retry_at_ms = 0;
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
            break;
        case MQTT_ST_DISCONNECTING:
            f->state = MQTT_ST_IDLE;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
            break;
        default:
            break;
        }
        break;

    case MQTT_EV_SOCK_CONNECTED:
        if (f->state == MQTT_ST_CONNECTING) {
            f->last_tx_ms = now_ms;
            act_push(f, MQTT_ACT_SEND_CONNECT, 0);
        }
        break;

    case MQTT_EV_SOCK_FAILED:
        if (f->state != MQTT_ST_IDLE && f->state != MQTT_ST_WAIT_RETRY) {
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            fsm_enter_retry(f, now_ms);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_EV_IO_ERROR:
        if (f->state == MQTT_ST_DISCONNECTING) {
            f->state = MQTT_ST_IDLE;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        } else if (f->state != MQTT_ST_IDLE && f->state != MQTT_ST_WAIT_RETRY) {
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            fsm_enter_retry(f, now_ms);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_EV_RECV_CONNACK: {
        const mqtt_connack_t *ca = (const mqtt_connack_t *)arg;
        /* arg 缺失按最坏情况（拒绝）处理：失败方向落在安全的一侧。 */
        uint8_t code = (ca != NULL) ? ca->ret_code : (uint8_t)0xFF;
        if (f->state != MQTT_ST_CONNECTING) break;
        f->connack_code    = code;
        f->session_present = (ca != NULL) ? ca->session_present : false;

        if (code == MQTT_CONNACK_ACCEPTED) {
            if (f->stat_connects > 0) f->stat_reconnects++;
            f->stat_connects++;
            f->state          = MQTT_ST_CONNECTED;
            f->retry_count    = 0;
            f->backoff_ms     = 0;
            f->retry_at_ms    = 0;
            f->pending_ping   = false;
            f->ping_sent_ms   = 0;
            f->pending_puback = 0;
            f->last_tx_ms     = now_ms;
            /* clean_session=1：会话不保留，重连后旧 pkt_id 无意义 → 清空 inflight。
             * 代价：上行事件在重连窗口内会丢，与规约 §5.10.1 的契约边界一致
             * （上行的权威记录是本地 safe.log，不是 MQTT 通道）。 */
            if (f->inflight != NULL) mqtt_inflight_init(f->inflight);
            if (f->pending_subscribe) act_push(f, MQTT_ACT_SEND_SUBSCRIBE, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        } else {
            /* ★ 协议性拒绝（如 0x05 未授权）**不进退避重连** —— 重连只会拿到
             * 同一个拒绝码，白白打 broker。停在 IDLE，交给上层决定
             * （记日志 / 提示凭据错误）。这是「区分错误类型」的考点。 */
            f->state        = MQTT_ST_IDLE;
            f->retry_at_ms  = 0;
            f->pending_ping = false;
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;
    }

    case MQTT_EV_RECV_SUBACK:
        (void)arg;
        f->pending_subscribe = false;
        break;

    case MQTT_EV_RECV_PUBLISH: {
        const mqtt_publish_rx_t *rx = (const mqtt_publish_rx_t *)arg;
        if (rx == NULL || f->state != MQTT_ST_CONNECTED) break;
        f->stat_rx_publish++;
        /* QoS1 才需要 PUBACK，且必须**入队成功**才发（入队失败由调用方
         * 调 mqtt_fsm_cancel_puback 放弃 → broker 重投）。 */
        if (rx->qos == 1u && rx->pkt_id != 0u) f->pending_puback = rx->pkt_id;
        act_push(f, MQTT_ACT_DELIVER, rx->pkt_id);
        break;
    }

    case MQTT_EV_RECV_PUBACK: {
        const uint16_t *pid = (const uint16_t *)arg;
        /* 迟到的 / 重复的 PUBACK：上层忽略即可，不是错误。 */
        if (pid != NULL && f->inflight != NULL) (void)mqtt_inflight_ack(f->inflight, *pid);
        break;
    }

    case MQTT_EV_RECV_PINGRESP:
        f->pending_ping = false;
        f->ping_sent_ms = 0;
        break;

    case MQTT_EV_TX_FLUSHED:
        f->last_tx_ms = now_ms;
        if (f->state == MQTT_ST_DISCONNECTING) {
            f->state = MQTT_ST_IDLE;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;
    }

    if (act_pop(f, out)) {
        if (out != NULL) out->next_wake_ms = mqtt_fsm_next_wake_ms(f, now_ms);
        return;
    }
    act_none(f, now_ms, out);
}

uint16_t mqtt_fsm_publish_begin(mqtt_fsm_t *f, const uint8_t *pkt, size_t len,
                                uint32_t now_ms, mqtt_fsm_out_t *out)
{
    if (f == NULL || pkt == NULL || len == 0u) { act_none(f, now_ms, out); return 0; }
    if (f->state != MQTT_ST_CONNECTED)         { act_none(f, now_ms, out); return 0; }
    if (f->inflight == NULL)                   { act_none(f, now_ms, out); return 0; }

    uint16_t id = mqtt_inflight_next_id(f->inflight);
    if (id == 0u) { act_none(f, now_ms, out); return 0; }

    /* 报文 > MQTT_INFLIGHT_PKT_MAX（512B）时入队必然失败 —— 调用方应在调用前
     * 判定并降级为 QoS0 直发（见设计 §3.8），这里只负责如实返回 0。 */
    if (mqtt_inflight_add(f->inflight, id, pkt, len, now_ms) != MQTT_OK) {
        act_none(f, now_ms, out);
        return 0;
    }

    f->last_tx_ms = now_ms;
    act_push(f, MQTT_ACT_SEND_PUBLISH, id);
    if (act_pop(f, out)) {
        if (out != NULL) out->next_wake_ms = mqtt_fsm_next_wake_ms(f, now_ms);
    } else {
        act_none(f, now_ms, out);
    }
    return id;
}

int mqtt_fsm_on_packet(mqtt_fsm_t *f, const mqtt_pkt_view_t *v,
                       uint32_t now_ms, mqtt_fsm_out_t *out)
{
    if (f == NULL || v == NULL) {
        act_none(f, now_ms, out);
        return MQTT_ERR_NULL;
    }

    switch (v->type) {
    case MQTT_PKT_CONNACK: {
        mqtt_connack_t ca;
        memset(&ca, 0, sizeof(ca));
        int r = mqtt_parse_connack(v, &ca.session_present, &ca.ret_code);
        if (r != MQTT_OK) { act_none(f, now_ms, out); return r; }
        mqtt_fsm_on_event(f, MQTT_EV_RECV_CONNACK, &ca, now_ms, out);
        return MQTT_OK;
    }
    case MQTT_PKT_SUBACK: {
        mqtt_suback_t sa;
        memset(&sa, 0, sizeof(sa));
        int r = mqtt_parse_suback(v, &sa.pkt_id, &sa.ret_code);
        if (r != MQTT_OK) { act_none(f, now_ms, out); return r; }
        mqtt_fsm_on_event(f, MQTT_EV_RECV_SUBACK, &sa, now_ms, out);
        return MQTT_OK;
    }
    case MQTT_PKT_PUBLISH: {
        char topic[FSM_TOPIC_CAP];
        mqtt_publish_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        rx.topic = topic;
        int r = mqtt_parse_publish(v, topic, sizeof(topic), &rx.payload, &rx.payload_len,
                                   &rx.pkt_id, &rx.dup, &rx.qos, &rx.retain);
        if (r != MQTT_OK) { act_none(f, now_ms, out); return r; }
        mqtt_fsm_on_event(f, MQTT_EV_RECV_PUBLISH, &rx, now_ms, out);
        return MQTT_OK;
    }
    case MQTT_PKT_PUBACK: {
        uint16_t pid = 0;
        int r = mqtt_parse_puback(v, &pid);
        if (r != MQTT_OK) { act_none(f, now_ms, out); return r; }
        mqtt_fsm_on_event(f, MQTT_EV_RECV_PUBACK, &pid, now_ms, out);
        return MQTT_OK;
    }
    case MQTT_PKT_PINGRESP:
        mqtt_fsm_on_event(f, MQTT_EV_RECV_PINGRESP, NULL, now_ms, out);
        return MQTT_OK;
    default:
        /* 本客户端不支持的报文（QoS2 的 PUBREC/PUBREL/PUBCOMP 等）：
         * 如实上报，让调用方决定（我们的订阅是 QoS1，正常不会遇到）。 */
        act_none(f, now_ms, out);
        return MQTT_ERR_UNSUPPORTED;
    }
}
