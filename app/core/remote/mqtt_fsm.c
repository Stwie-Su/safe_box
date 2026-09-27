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
#define FSM_DEF_SUBACK_TIMEOUT_MS  5000u

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

/* 退避时长 = base << min(streak, 6)，再钉在封顶值上；最后叠 [0.75b, b] 的抖动。
 *
 * ★ streak 是**参数**、不是 retry_count：链路层失败用 retry_count，订阅层失败
 * 用 suback_reject_streak（缺陷 #11）。两条阶梯必须分开，否则 CONNACK(0) 成功
 * 时把 retry_count 清零，会让订阅失败永远从 500ms 起步。 */
static uint32_t fsm_backoff_calc(mqtt_fsm_t *f, uint8_t streak)
{
    uint32_t base = f->cfg.backoff_base_ms ? f->cfg.backoff_base_ms : FSM_DEF_BACKOFF_BASE_MS;
    uint32_t max  = f->cfg.backoff_max_ms  ? f->cfg.backoff_max_ms  : FSM_DEF_BACKOFF_MAX_MS;

    /* 位移封顶 6（原为 5）。★ 为什么必须是 6：
     * base=500 时 500<<5 = 16000 < 默认封顶 30000（见 cfg_default），位移先到顶、
     * 封顶后到 —— 结果 backoff_max_ms=30s 这条配置**一次都不生效**，实际封顶
     * 停在 16s，而 cfg_default 里关于「30s 而非 8s」的论证全部落空。
     * 改成 6：500<<6 = 32000 > 30000 → 命中封顶，与注释一致。
     *
     * 代价（写下来，别到时候忘了是自己选的）：第 7 次及以后的重连间隔从 16s
     * 变成 30s，broker 恢复后设备的"感知延迟"变长。可接受 —— 半开连接本来就有
     * LWT 兜底，UI 侧还有 5s 一次的状态快照，用户不会看到"离线但界面假在线"。
     *
     * 已知约束：若把 backoff_max_ms 调到 >32s，这里的位移封顶 6 也要跟着调大，
     * 否则又会退化成"位移先封顶、封顶不生效"。默认配置下 30s < 32s，安全。 */
    uint8_t  sh   = (streak > 6) ? (uint8_t)6 : streak;

    uint32_t b = base << sh;                    /* 指数增长，最多 <<6（64 倍） */
    if (b == 0u || b > max) b = max;            /* 溢出或超封顶 → 钉在封顶值 */

    /* 抖动：delay ∈ [0.75b, b]。
     * 目的：避免同一 broker 上一批设备同时重连形成同步风暴（惊群）。 */
    uint32_t q = b / 4u;
    return (b - q) + (fsm_rand(f) % (q + 1u));
}

static uint32_t fsm_backoff_next(mqtt_fsm_t *f)
{
    return fsm_backoff_calc(f, f->retry_count);
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
    /* ★ 等 SUBACK 的定时器必须与"已建连"一起清：断链后旧的那次 SUBSCRIBE 永远
     * 等不到回包，不清就会在重连后用**上一次发送的时刻**判超时，可能在刚连上的
     * 瞬间就误判一次断链（表现为「重连后立刻又断」，极难定位）。 */
    f->pending_suback = false;
    f->sub_sent_ms    = 0;
}

/* ★ 订阅失败专用（缺陷 #11）：与 fsm_enter_retry 唯一的区别是退避用
 * **suback_reject_streak** 而不是 retry_count。
 *
 * 为什么必须分开：见 mqtt_fsm.h 里 suback_reject_streak 的注释 —— CONNACK(0)
 * 会把 retry_count 清零，而 SUBACK 失败恰好发生在 CONNACK 成功之后，于是退避
 * 值恒等于 500ms。实测 30s 内 69 次重连就是这么来的。
 *
 * 这里**不动** retry_count：链路层没失败（CONNACK 是成功的），把它累加会让
 * 下一次真正的链路故障一上来就按很长的退避等 —— 那是把订阅问题误记到链路账上。 */
static void fsm_enter_retry_suback(mqtt_fsm_t *f, uint32_t now_ms)
{
    uint32_t d = fsm_backoff_calc(f, f->suback_reject_streak);
    f->backoff_ms     = d;
    f->retry_at_ms    = now_ms + d;
    if (f->suback_reject_streak < 0xFFu) f->suback_reject_streak++;
    f->state          = MQTT_ST_WAIT_RETRY;
    f->pending_ping   = false;
    f->ping_sent_ms   = 0;
    f->pending_puback = 0;
    f->pending_suback = false;
    f->sub_sent_ms    = 0;
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
    /* SUBACK 超时 5s：等太久会让「已连但失聪」的窗口过长，太短会在 busy broker
     * 上误判断链。语义与 retry_timeout_ms 同族（"发出去的控制报文没被确认"），
     * 故取同一量级。超时只触发重连，代价可控。 */
    cfg->suback_timeout_ms = 5000u;
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
    /* 0xFF = 尚未收到过合法 SUBACK。★ 不能用 0 表示"未定"：0 本身就是一个
     * 合法的授予 QoS（broker 把我们的 QoS1 降到 QoS0 时就是 0）。 */
    f->granted_qos = 0xFFu;
    f->suback_reject_streak = 0;   /* 见 fsm_enter_retry_suback：与 retry_count 分开的两条阶梯 */
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

        /* ★ SUBACK 超时定时器必须在这里登记（缺陷 #12 的"另一半"）：
         * next_wake_ms 是 net 线程 poll 超时的**唯一**来源，定时器不登记就等于
         * 不存在 —— 状态机只有在别的事情（keepalive / 下行报文）唤醒它时才会
         * 顺带检查，而那可能是 20s 之后的事。这类「加了定时逻辑却忘了登记」
         * 是本项目最容易犯的错，改状态机必须同时问一句：next_wake_ms 改了吗？ */
        if (f->pending_suback) {
            uint32_t st = f->cfg.suback_timeout_ms ? f->cfg.suback_timeout_ms
                                                   : FSM_DEF_SUBACK_TIMEOUT_MS;
            uint32_t es = now_ms - f->sub_sent_ms;
            uint32_t rs = (es >= st) ? 0u : (st - es);
            if (rs < best) best = rs;
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
        /* (b2) SUBACK 超时 → 按订阅失败处理：关链路 + 退避重连（缺陷 #12）。
         *
         * ★ 为什么必须有这条：没有它，broker 因任何原因不回 SUBACK 时，设备会
         *   永远停在 CONNECTED —— pending_subscribe 在发出 SUBSCRIBE 时已经清
         *   掉，keepalive 正常、PINGREQ/PINGRESP 照常、界面显示"在线"、
         *   一条下行都收不到、**而且零报错**。这正是「已连但失聪」最隐蔽的一种
         *   （实测：SUBSCRIBE 只发 1 次，之后设备再无任何动作）。
         *
         * 处置与被拒(0x80)一致：断链重连。重连会重发 SUBSCRIBE（WAIT_RETRY
         * →CONNECTING 处已置 pending_subscribe），所以能自愈。退避走
         * suback_reject_streak 那条独立阶梯 —— 超时同样要限流，否则一个不回
         * SUBACK 的 broker 会被我们以最快频率反复重连。 */
        if (f->pending_suback) {
            uint32_t st = f->cfg.suback_timeout_ms ? f->cfg.suback_timeout_ms
                                                   : FSM_DEF_SUBACK_TIMEOUT_MS;
            if ((uint32_t)(now_ms - f->sub_sent_ms) >= st) {
                f->stat_suback_timeout++;
                f->pending_suback = false;
                act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
                fsm_enter_retry_suback(f, now_ms);
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
            f->granted_qos      = 0xFFu;   /* 与 CONNACK 分支同口径（#13）：新会话没有授予 QoS */
            f->suback_reject_streak = 0;   /* 显式重启：上一次的"连续失败"不该延续到新会话 */
            f->pending_suback   = false;
            f->sub_sent_ms      = 0;
            f->last_tx_ms        = now_ms;
            act_push(f, MQTT_ACT_OPEN_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;

    case MQTT_EV_STOP:
        f->pending_puback = 0;
        f->pending_ping   = false;
        f->pending_suback = false;   /* 停止后不再等任何回包 */
        f->sub_sent_ms    = 0;
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
            /* ★ 每次建连都必须把"上一次连接"的授予 QoS 清掉（缺陷 #13）。
             * 与 pending_subscribe 那个 P0 是**同一类**漏网：重连时该重置却没重置。
             * 后果：重连后若新连接还没收到 SUBACK，granted_qos 仍留着上一次的
             * 值，任何"按授予 QoS 决定要不要等 PUBACK"的判断都会用到脏数据 ——
             * 这种 bug 只在特定时序下出现，排查时极难联想到"是上一次连接的残留"。 */
            f->granted_qos = 0xFFu;
            f->pending_suback = false;   /* 上一次等 SUBACK 的定时器（重连后无意义） */
            f->sub_sent_ms    = 0;
            if (f->pending_subscribe) {
                act_push(f, MQTT_ACT_SEND_SUBSCRIBE, 0);
                /* 起 SUBACK 超时定时器（#12）。必须在 push 的同一处置位：
                 * 两处逻辑一旦分开，将来新增一个"发 SUBSCRIBE"的地方就会漏掉
                 * 定时器 —— 而漏掉的表现是"沉默失聪"，日志里什么都没有。 */
                f->pending_suback = true;
                f->sub_sent_ms    = now_ms;
            }
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        } else if (code == 0x03u) {
            /* 0x03（Server unavailable）是**瞬时**故障：broker 正在重启或过载，
             * 过一会儿再来大概率就成功了 —— 值得重试。
             * 处置与其它连接失败一致：关链路 → 退避重连。 */
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            fsm_enter_retry(f, now_ms);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        } else {
            /* ★ 其余返回码都是**永久性**失败，重试只会拿到同一个拒绝码：
             *   0x01 协议版本不支持 / 0x02 client id 被拒 —— 设备侧配置问题；
             *   0x04 用户名密码错 / 0x05 未授权 —— 凭据问题。
             * 原实现把它们和 0x03 混为一谈（统一停在 IDLE 不重试），方向碰巧
             * 对了一半；这里显式区分，并把「不重试」的理由写清楚。
             *
             * 上报通道：connack_code 随 NOTIFY_STATE 出门（见 do_notify 打印的
             * "disconnected (connack=N)"），上层据此提示「凭据错误」而不是
             * 让用户对着一个沉默的离线设备猜。
             * 恢复路径：**当前进程内没有**（诚实表述，别再写成"上层重新发
             * MQTT_EV_START"了 —— 那句话曾经是错的：MQTT_EV_START 全进程只在
             * mqtt_start() 里发一次，grep 不到第二个调用点，所以"改完配置重发
             * START"至今只是一句注释，没有实现）。也就是说：
             *     凭据写错 / ACL 变更 ⇒ 设备停在 IDLE，**直到进程重启**。
             *
             * 为什么**不**现在补一个 mqtt_reconnect_now() 之类的恢复入口：
             *   1. 永久错误的成因是"配置错了"，而本项目的 MQTT 配置（host / 端口 /
             *      用户名 / 口令 / TLS）全部在 mqtt_start() **之前**注入、运行期不
             *      可变。运行期重连只会拿到同一个拒绝码 —— 恢复入口没有真实需求。
             *   2. 加一个跨线程入口就要回答"谁来调、配置从哪来、失败怎么回"，
             *      而现在**没有任何调用方**（UI 也没有"重连"按钮）。没有调用方的
             *      API 就是死代码 —— 本仓已经有过一次教训（mqtt_stats_ex 那组
             *      getter 至今零调用者）。
             *   3. 真要支持运行期改配置，正确顺序是"先把配置改成可热更新"，
             *      再谈恢复入口 —— 那是另一个特性，不该混在缺陷修复里。
             * 所以这里选 (b)：不改行为，只把注释改成实话，并让 #15 的日志明确
             * 告诉用户"请修正 MQTT 配置后重启设备"，而不是让他对着静默的设备猜。
             *
             * ★ 为什么不新增 FATAL 状态或 f->fatal 标志：「已停止、需人工介入」
             *   与 IDLE 的语义（未启动/已停止）本就是同一件事，区分「从未启动」
             *   与「因永久错误停止」所需的信息已经由 f->connack_code 承载（非 0
             *   即后者）。新增状态要同步改 state_name / next_wake_ms 等多处分支，
             *   多一处状态就多一类漏改的迁移 —— 收益为零，风险不为零。
             *
             * ★★ 为什么这里**必须**产出 CLOSE_SOCKET（缺陷 #10，实测 net 线程
             *   99.7% CPU）：状态迁移必须闭合它持有的资源。只把 state 改成 IDLE
             *   而不关 fd，后果链是：
             *       g_sock 还在 → broker 按 MQTT 规范**必须**关这条连接 →
             *       poll 的 POLLIN/POLLHUP 永久就绪（对端 EOF 一直在那儿）→
             *       读返回 0 → 上层发 MQTT_EV_IO_ERROR →
             *       而 IO_ERROR 在 IDLE 下**两个分支都不成立**（见上）→
             *       什么都没做 → 回到 poll → 立刻又就绪 → **空转打满一个核**。
             *   一句话：状态机说"我停了"，但它占着的 fd 还在喂事件给驱动它的线程。
             *
             *   为什么它值得排在最前面修：① 唯一一个实测到单核打满的，没有解释
             *   空间；② 触发条件最容易被真实用户踩到 —— SAFE_MQTT_USER/PASS
             *   填错就行，**不需要攻击者**；③ 修法只有一行；④ 「状态迁移必须
             *   闭合资源」是状态机设计的基本功，比"又加了一个校验"高一个层次。
             *   旁边 0x03 瞬时分支有 CLOSE_SOCKET 而这里没有，属于漏改而非设计。 */
            f->state        = MQTT_ST_IDLE;
            f->retry_at_ms  = 0;
            f->pending_ping = false;
            f->pending_suback = false;
            f->sub_sent_ms    = 0;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        }
        break;
    }

    case MQTT_EV_RECV_SUBACK: {
        const mqtt_suback_t *sa = (const mqtt_suback_t *)arg;
        /* arg 缺失按**最坏情况**（订阅被拒）处理：失败方向落在安全的一侧 ——
         * 「以为订阅成功了」比「以为订阅失败了」危险得多。 */
        uint8_t rc = (sa != NULL) ? sa->ret_code : (uint8_t)0x80u;

        f->pending_subscribe = false;
        f->pending_suback    = false;   /* 无论成败，这一轮"等 SUBACK"都结束了 */
        f->sub_sent_ms       = 0;

        if (rc == 0x80u || rc > 2u) {
            /* 0x80 = broker 拒绝订阅；>2 不是合法 QoS（合法值只有 0/1/2）。
             *
             * ★ 为什么必须当失败处理（原实现是 `(void)arg;` 直接丢掉返回码）：
             *   这与「重连不重订阅」是**同一个失效模式、不同路径** —— 设备以为
             *   自己已订阅 safe/cmd，实际 broker 一条下行都不会推，界面照样显示
             *   "在线"，而且**零报错**。对保险柜就是「远程开锁永远不响应」。
             *
             * ── 判"瞬时"还是"永久"？（下面这段是刻意写下来的论证，不是结论）──
             * 结论：**判瞬时，但走独立退避阶梯**；不引入"永久停止"。
             *
             * 为什么不像 CONNACK 0x04/0x05 那样判永久停止：
             *   0x04/0x05 是**凭据**被拒，只由设备侧配置（用户名/密码/client id）
             *   和 broker 的静态认证表决定 —— 人不改配置，重连一万次都是同一个码，
             *   所以停在那儿是对的（45f4663 就是这么分的）。
             *   而 0x80 是**授权**决定，由 broker 在 SUBSCRIBE 这一刻做出，
             *   它可以在**设备侧零改动**的情况下变化：ACL 表热加载、策略下发、
             *   topic 迁移、broker 半配置状态。也就是说 0x80 有真实的自愈可能。
             *
             * 那为什么不能"发现被拒就一直重试"：
             *   因为退避若被 CONNACK(0) 每次清零（缺陷 #11），重连间隔恒等于
             *   500ms 最小值 → 实测 30s 内 69 次重连的固定周期风暴。
             *
             * 于是取两者之长：判瞬时（保留自愈），但退避走**独立的**
             * suback_reject_streak 阶梯，连续被拒时一路爬到封顶 30s。
             * 收敛后的代价上界是 2 次/分钟，与"停止"同量级，却保留了
             * 「ACL 一修好设备立刻自己恢复」这条关键性质 —— 对保险柜而言
             * "永远停止到重启"比"每 30s 试一次"危险得多。
             *
             * 为什么**不**在连续 N 次后转永久停止：那会把"broker 侧临时配错"
             * 判成"必须重启设备"，把可自愈的故障变成需要人工到场的故障。
             * 本设备的红线是"绝不静默失效"，而持续重试 + 计数器外露（#14）
             * 已经让运维看得见，不需要用"停止"来发声。 */
            f->stat_suback_reject++;
            act_push(f, MQTT_ACT_CLOSE_SOCKET, 0);
            fsm_enter_retry_suback(f, now_ms);
            act_push(f, MQTT_ACT_NOTIFY_STATE, 0);
        } else {
            /* rc ∈ {0,1,2} = broker 授予的 QoS。它可能**低于**本端申请的 QoS1
             * （MQTT_SUB_QOS，见 mqtt_client.c）—— 降级是协议允许的、可接受，
             * 但必须**记下来**：否则「申请 QoS1 实际只拿到 QoS0」在排障时完全
             * 不可见。本层不能打印（文件头三条铁律），故落成字段供上层/单测读。 */
            f->granted_qos = rc;
            /* 订阅成功了 → 连续失败计数归零（它记的是"连续"，不是"累计"）。 */
            f->suback_reject_streak = 0;
        }
        break;
    }

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
