/**
 * @file test_mqtt_fsm.c
 * R9 T03：MQTT 连接状态机单元测试（**确定性** —— 时间由参数注入，不 sleep）。
 *
 * 覆盖设计文档 §2.9 列的 9 条，外加背压（入队失败不回 PUBACK）一条：
 *   1. 启动序列 IDLE→START→OPEN_SOCKET→SOCK_CONNECTED→SEND_CONNECT
 *   2. CONNACK(0) → CONNECTED + SEND_SUBSCRIBE + NOTIFY_STATE
 *   3. CONNACK(5) → IDLE + NOTIFY_STATE(5)，且**不产生** OPEN_SOCKET（不重连）
 *   4. keepalive 到期 → SEND_PINGREQ；不喂 PINGRESP → 再推进 ping_timeout
 *      → CLOSE_SOCKET + WAIT_RETRY + stat_ping_timeout == 1
 *   5. 退避序列：连续失败 8 次，断言落在 [0.75b, b] 且封顶 30s；同种子两次结果一致
 *   6. 重传：入队一条 inflight → 推进 retry_timeout → RETRANSMIT；
 *      三次后 → DROP_INFLIGHT + stat_dropped == 1
 *   7. 重连后 mqtt_inflight_count == 0（clean_session=1，旧 pkt_id 无意义）
 *      ★ 且重连后**必须再发一次 SUBSCRIBE**（P0 回归：重连不重订阅 =
 *        设备从此收不到任何下行指令，且零报错）
 *   8. on_packet 分派（CONNACK / PUBLISH / PUBACK / PINGRESP）与不支持类型
 *   9. 背压：下行入队失败 → cancel_puback 后不再产出 SEND_PUBACK
 *  10. NULL / 非法入参不崩
 */
#include "test_util.h"

#include <string.h>

#include "core/remote/mqtt_fsm.h"
#include "core/remote/mqtt_codec.h"

#define T0 1000u

/* 消费一次事件产出的**全部**动作，按序记进 acts[]，返回动作个数。
 * ★ 调用方（T04 的 net 线程）必须这样排空 —— 状态机的动作 FIFO 容量只有 4，
 *   不排空就会溢出丢动作。这里把它做成测试里的默认姿势。 */
static size_t drain(mqtt_fsm_t *f, uint32_t now, mqtt_fsm_out_t *out,
                    mqtt_fsm_action_t *acts, size_t max)
{
    size_t n = 0;
    while (out->act != MQTT_ACT_NONE) {
        if (acts != NULL && n < max) acts[n] = out->act;
        n++;
        mqtt_fsm_tick(f, now, out);
    }
    return n;
}

static void fsm_setup(mqtt_fsm_t *f, mqtt_inflight_t *q)
{
    mqtt_fsm_cfg_t cfg;
    mqtt_fsm_cfg_default(&cfg);
    cfg.rng_seed = 0x2545F491u;
    mqtt_fsm_init(f, &cfg, q);
}

/* 走完 START → SOCK_CONNECTED → CONNACK(0)，停在 CONNECTED 且动作已排空 */
static void bring_up(mqtt_fsm_t *f, mqtt_inflight_t *q)
{
    mqtt_fsm_out_t out;
    mqtt_connack_t ca;
    fsm_setup(f, q);
    mqtt_fsm_on_event(f, MQTT_EV_START, NULL, T0, &out);
    drain(f, T0, &out, NULL, 0);
    mqtt_fsm_on_event(f, MQTT_EV_SOCK_CONNECTED, NULL, T0, &out);
    drain(f, T0, &out, NULL, 0);
    memset(&ca, 0, sizeof(ca));
    mqtt_fsm_on_event(f, MQTT_EV_RECV_CONNACK, &ca, T0, &out);
    drain(f, T0, &out, NULL, 0);
}

/* 一条最小可用的 QoS1 PUBLISH 报文（够 inflight 用，不关心内容） */
static size_t make_publish(uint8_t *buf, size_t cap, uint16_t pkt_id)
{
    size_t n = 0;
    int r = mqtt_pack_publish(buf, cap, &n, "safe/log",
                              (const uint8_t *)"{}", 2u, pkt_id, false, 1, false);
    if (r != MQTT_OK) return 0;
    return n;
}

int main(void)
{
    /* ---- 1. 启动序列 ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        size_t n;
        mqtt_inflight_init(&q);
        fsm_setup(&f, &q);
        CHECK(f.state == MQTT_ST_IDLE);
        CHECK(f.pending_subscribe == true);      /* clean_session=1：建连后要重发 SUBSCRIBE */
        CHECK(mqtt_fsm_next_wake_ms(&f, T0) > 0);

        mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, T0, &out);
        n = drain(&f, T0, &out, acts, 8);
        CHECK(n == 2);
        CHECK(acts[0] == MQTT_ACT_OPEN_SOCKET);
        CHECK(acts[1] == MQTT_ACT_NOTIFY_STATE);
        CHECK(f.state == MQTT_ST_CONNECTING);

        mqtt_fsm_on_event(&f, MQTT_EV_SOCK_CONNECTED, NULL, T0, &out);
        n = drain(&f, T0, &out, acts, 8);
        CHECK(n == 1);
        CHECK(acts[0] == MQTT_ACT_SEND_CONNECT);
        CHECK(f.state == MQTT_ST_CONNECTING);    /* 要等 CONNACK 才算连上 */
    }

    /* ---- 2. CONNACK(0) → CONNECTED + SEND_SUBSCRIBE + NOTIFY_STATE ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        mqtt_connack_t ca;
        size_t n;
        mqtt_inflight_init(&q);
        fsm_setup(&f, &q);
        mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);
        mqtt_fsm_on_event(&f, MQTT_EV_SOCK_CONNECTED, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);

        memset(&ca, 0, sizeof(ca));
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_CONNACK, &ca, T0, &out);
        CHECK(f.state == MQTT_ST_CONNECTED);
        n = drain(&f, T0, &out, acts, 8);
        CHECK(n == 2);
        CHECK(acts[0] == MQTT_ACT_SEND_SUBSCRIBE);
        CHECK(acts[1] == MQTT_ACT_NOTIFY_STATE);
        CHECK(out.connack_code == 0);
        CHECK(f.stat_connects == 1);
        CHECK(f.stat_reconnects == 0);

        /* SUBACK 到达 → 清 pending_subscribe */
        {
            mqtt_suback_t sa;
            memset(&sa, 0, sizeof(sa));
            sa.pkt_id = 1;
            mqtt_fsm_on_event(&f, MQTT_EV_RECV_SUBACK, &sa, T0, &out);
            CHECK(out.act == MQTT_ACT_NONE);
        }
        CHECK(f.pending_subscribe == false);
    }

    /* ---- 3. CONNACK(5) → IDLE + NOTIFY_STATE(5)，**不重连** ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        mqtt_connack_t ca;
        size_t n;
        mqtt_inflight_init(&q);
        fsm_setup(&f, &q);
        mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);
        mqtt_fsm_on_event(&f, MQTT_EV_SOCK_CONNECTED, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);

        memset(&ca, 0, sizeof(ca));
        ca.ret_code = 5;                       /* 0x05 未授权 */
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_CONNACK, &ca, T0, &out);
        CHECK(f.state == MQTT_ST_IDLE);
        n = drain(&f, T0, &out, acts, 8);
        CHECK(n == 1);
        CHECK(acts[0] == MQTT_ACT_NOTIFY_STATE);
        CHECK(out.connack_code == 5);
        CHECK(f.stat_connects == 0);           /* 未算作一次成功连接 */
        /* ★ 关键：不进 WAIT_RETRY，也不产出 OPEN_SOCKET —— 重连只会拿到同一个拒绝码 */
        CHECK(f.retry_at_ms == 0);
        mqtt_fsm_tick(&f, T0 + 60000u, &out);
        CHECK(out.act == MQTT_ACT_NONE);
        CHECK(f.state == MQTT_ST_IDLE);
    }

    /* ---- 4. keepalive → PINGREQ；PING 超时 → CLOSE_SOCKET + WAIT_RETRY ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        size_t n;
        uint32_t t_ka, t_pt;

        mqtt_inflight_init(&q);
        bring_up(&f, &q);

        t_ka = T0 + (uint32_t)f.cfg.keepalive_s * 1000u;
        mqtt_fsm_tick(&f, t_ka - 1u, &out);
        CHECK(out.act == MQTT_ACT_NONE);
        mqtt_fsm_tick(&f, t_ka, &out);
        CHECK(out.act == MQTT_ACT_SEND_PINGREQ);
        CHECK(f.pending_ping == true);
        CHECK(f.ping_sent_ms == t_ka);

        /* 没收到 PINGRESP：再推进 ping_timeout → 判半开连接 */
        t_pt = t_ka + f.cfg.ping_timeout_ms;
        mqtt_fsm_tick(&f, t_pt - 1u, &out);
        CHECK(out.act == MQTT_ACT_NONE);
        CHECK(f.state == MQTT_ST_CONNECTED);
        mqtt_fsm_tick(&f, t_pt, &out);
        n = drain(&f, t_pt, &out, acts, 8);
        CHECK(n == 2);
        CHECK(acts[0] == MQTT_ACT_CLOSE_SOCKET);
        CHECK(acts[1] == MQTT_ACT_NOTIFY_STATE);
        CHECK(f.state == MQTT_ST_WAIT_RETRY);
        CHECK(f.stat_ping_timeout == 1);
        CHECK(f.retry_at_ms > t_pt);

        /* 反之：PINGRESP 及时到达 → 清标志、不判超时 */
        mqtt_inflight_init(&q);
        bring_up(&f, &q);
        t_ka = T0 + (uint32_t)f.cfg.keepalive_s * 1000u;
        mqtt_fsm_tick(&f, t_ka, &out);
        CHECK(out.act == MQTT_ACT_SEND_PINGREQ);
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_PINGRESP, NULL, t_ka + 1000u, &out);
        CHECK(f.pending_ping == false);
        /* 尚未到一个 keepalive 周期，也不该有 ping 超时 */
        mqtt_fsm_tick(&f, t_ka + 19000u, &out);
        CHECK(out.act == MQTT_ACT_NONE);
        CHECK(f.state == MQTT_ST_CONNECTED);
        CHECK(f.stat_ping_timeout == 0);
    }

    /* ---- 5. 退避序列：区间 + 封顶 + 同种子确定性 ---- */
    {
        uint32_t seq_a[8];
        uint32_t seq_b[8];
        const uint32_t base = 500u, cap = 30000u;
        int round;
        for (round = 0; round < 2; round++) {
            mqtt_fsm_t f;
            mqtt_inflight_t q;
            mqtt_fsm_out_t out;
            mqtt_fsm_cfg_t cfg;
            uint32_t now = T0;
            mqtt_inflight_init(&q);
            mqtt_fsm_cfg_default(&cfg);
            cfg.backoff_base_ms = base;
            cfg.backoff_max_ms  = cap;
            cfg.rng_seed        = 0x2545F491u;
            mqtt_fsm_init(&f, &cfg, &q);

            mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, now, &out);
            drain(&f, now, &out, NULL, 0);
            for (int i = 0; i < 8; i++) {
                mqtt_fsm_on_event(&f, MQTT_EV_SOCK_FAILED, NULL, now, &out);
                drain(&f, now, &out, NULL, 0);
                CHECK(f.state == MQTT_ST_WAIT_RETRY);

                uint32_t b = base << ((i > 5) ? 5 : i);
                if (b == 0u || b > cap) b = cap;
                uint32_t d = f.retry_at_ms - now;
                CHECK(d >= b - b / 4u);          /* ≥ 0.75b */
                CHECK(d <= b);                   /* ≤ b     */
                CHECK(f.backoff_ms == d);

                if (round == 0) seq_a[i] = d; else seq_b[i] = d;

                /* 退避到期 → OPEN_SOCKET 重连 */
                now = f.retry_at_ms;
                mqtt_fsm_tick(&f, now, &out);
                CHECK(out.act == MQTT_ACT_OPEN_SOCKET);
                drain(&f, now, &out, NULL, 0);
                CHECK(f.state == MQTT_ST_CONNECTING);
            }
        }
        for (int i = 0; i < 8; i++) CHECK(seq_a[i] == seq_b[i]);   /* 同种子 → 同序列 */
        CHECK(seq_a[6] <= cap);                                    /* 封顶生效 */
        CHECK(seq_a[7] <= cap);
        CHECK(seq_a[0] <= 500u);
        CHECK(seq_a[1] >  seq_a[0]);                               /* 指数增长可见 */
        CHECK(seq_a[2] >  seq_a[1]);
        CHECK(seq_a[3] >  seq_a[2]);
    }

    /* ---- 6. 重传 → RETRANSMIT；用尽 → DROP_INFLIGHT ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        uint8_t pkt[256];
        size_t plen;
        uint16_t id;

        mqtt_inflight_init(&q);
        bring_up(&f, &q);

        plen = make_publish(pkt, sizeof(pkt), 7);
        CHECK(plen > 0);
        id = mqtt_fsm_publish_begin(&f, pkt, plen, T0, &out);
        CHECK(id != 0);
        CHECK(out.act == MQTT_ACT_SEND_PUBLISH);
        CHECK(mqtt_inflight_count(&q) == 1);

        mqtt_fsm_tick(&f, T0 + f.cfg.retry_timeout_ms, &out);
        CHECK(out.act == MQTT_ACT_RETRANSMIT);
        CHECK(out.pkt_id == id);
        CHECK(f.stat_retransmit == 1);

        mqtt_fsm_tick(&f, T0 + 2u * f.cfg.retry_timeout_ms, &out);
        CHECK(out.act == MQTT_ACT_RETRANSMIT);
        CHECK(f.stat_retransmit == 2);

        /* 第三次：retry 达 MQTT_INFLIGHT_MAX_RETRY → 丢弃并记 stat_dropped */
        mqtt_fsm_tick(&f, T0 + 3u * f.cfg.retry_timeout_ms, &out);
        CHECK(out.act == MQTT_ACT_DROP_INFLIGHT);
        CHECK(f.stat_dropped == 1);
        CHECK(mqtt_inflight_count(&q) == 0);

        /* 收到 PUBACK 则不再重传 */
        mqtt_inflight_init(&q);
        bring_up(&f, &q);
        id = mqtt_fsm_publish_begin(&f, pkt, plen, T0, &out);
        CHECK(id != 0);
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_PUBACK, &id, T0 + 100u, &out);
        CHECK(mqtt_inflight_count(&q) == 0);
        mqtt_fsm_tick(&f, T0 + 100u + f.cfg.retry_timeout_ms, &out);
        CHECK(out.act == MQTT_ACT_NONE);
        CHECK(f.stat_retransmit == 0);
    }

    /* ---- 7. 重连后 inflight 清空 ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        mqtt_connack_t ca;
        mqtt_suback_t sa;
        uint8_t pkt[256];
        size_t plen;
        size_t n;
        uint32_t now;

        mqtt_inflight_init(&q);
        bring_up(&f, &q);
        /* ★ 必须先把第一次订阅**走完**（收到 SUBACK → 清 pending_subscribe）。
         * 这不是走过场：缺陷恰恰是「订阅成功过一次之后就再也不重订阅」——
         * 若跳过 SUBACK，pending_subscribe 恒为 true，下面的断言会平凡通过，
         * 变成一条永远抓不到缺陷的空测试（本项目踩过的「只见通过」陷阱）。 */
        memset(&sa, 0, sizeof(sa));
        sa.pkt_id = 1;
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_SUBACK, &sa, T0, &out);
        drain(&f, T0, &out, NULL, 0);
        CHECK(f.pending_subscribe == false);

        plen = make_publish(pkt, sizeof(pkt), 11);
        CHECK(mqtt_fsm_publish_begin(&f, pkt, plen, T0, &out) != 0);
        CHECK(mqtt_inflight_count(&q) == 1);

        /* 链路断 → 退避 → 重连 → CONNACK(0) */
        mqtt_fsm_on_event(&f, MQTT_EV_IO_ERROR, NULL, T0 + 10u, &out);
        drain(&f, T0 + 10u, &out, NULL, 0);
        CHECK(f.state == MQTT_ST_WAIT_RETRY);
        now = f.retry_at_ms;
        mqtt_fsm_tick(&f, now, &out);
        CHECK(out.act == MQTT_ACT_OPEN_SOCKET);
        drain(&f, now, &out, NULL, 0);
        /* ★ P0 回归断言：进入重连的那一刻就要把「待订阅」重新置位 ——
         * 否则 CONNACK 后不会再发 SUBSCRIBE（clean_session=1 时 broker 已丢
         * 订阅关系），设备从此收不到任何下行指令且没有任何报错。 */
        CHECK(f.pending_subscribe == true);
        mqtt_fsm_on_event(&f, MQTT_EV_SOCK_CONNECTED, NULL, now, &out);
        drain(&f, now, &out, NULL, 0);
        memset(&ca, 0, sizeof(ca));
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_CONNACK, &ca, now, &out);
        n = drain(&f, now, &out, acts, 8);
        CHECK(n == 2);
        CHECK(acts[0] == MQTT_ACT_SEND_SUBSCRIBE);   /* ★ 第二次连接也要订阅 */

        CHECK(f.state == MQTT_ST_CONNECTED);
        /* clean_session=1：旧 pkt_id 无意义 → 必须清空 */
        CHECK(mqtt_inflight_count(&q) == 0);
        CHECK(f.stat_reconnects == 1);
        CHECK(f.stat_connects == 2);
        CHECK(f.retry_count == 0);
    }

    /* ---- 8. on_packet 分派 ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_fsm_action_t acts[8];
        mqtt_pkt_view_t v;
        size_t used = 0;
        size_t n;

        mqtt_inflight_init(&q);
        fsm_setup(&f, &q);
        mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);
        mqtt_fsm_on_event(&f, MQTT_EV_SOCK_CONNECTED, NULL, T0, &out);
        drain(&f, T0, &out, NULL, 0);

        /* CONNACK(0) 字节：0x20 0x02 0x00 0x00 */
        {
            const uint8_t b[4] = { 0x20, 0x02, 0x00, 0x00 };
            CHECK(mqtt_parse(b, sizeof(b), &v, &used) == MQTT_OK);
            CHECK(mqtt_fsm_on_packet(&f, &v, T0, &out) == MQTT_OK);
            CHECK(f.state == MQTT_ST_CONNECTED);
            n = drain(&f, T0, &out, acts, 8);
            CHECK(n == 2);
            CHECK(acts[0] == MQTT_ACT_SEND_SUBSCRIBE);
            CHECK(acts[1] == MQTT_ACT_NOTIFY_STATE);
        }
        /* PINGRESP：0xD0 0x00 */
        {
            const uint8_t b[2] = { 0xD0, 0x00 };
            mqtt_fsm_tick(&f, T0 + (uint32_t)f.cfg.keepalive_s * 1000u, &out);
            CHECK(out.act == MQTT_ACT_SEND_PINGREQ);
            CHECK(mqtt_parse(b, sizeof(b), &v, &used) == MQTT_OK);
            CHECK(mqtt_fsm_on_packet(&f, &v, T0, &out) == MQTT_OK);
            CHECK(f.pending_ping == false);
        }
        /* 下行 PUBLISH(qos1) → DELIVER，随后 tick 产出 SEND_PUBACK */
        {
            uint8_t pb[256];
            size_t pn = 0;
            CHECK(mqtt_pack_publish(pb, sizeof(pb), &pn, "safe/cmd",
                                    (const uint8_t *)"{\"cmd\":\"query_status\"}", 22u,
                                    7, false, 1, false) == MQTT_OK);
            CHECK(mqtt_parse(pb, pn, &v, &used) == MQTT_OK);
            CHECK(mqtt_fsm_on_packet(&f, &v, T0, &out) == MQTT_OK);
            CHECK(out.act == MQTT_ACT_DELIVER);
            CHECK(out.pkt_id == 7);
            CHECK(f.stat_rx_publish == 1);
            mqtt_fsm_tick(&f, T0, &out);
            CHECK(out.act == MQTT_ACT_SEND_PUBACK);
            CHECK(out.pkt_id == 7);
        }
        /* PUBACK：0x40 0x02 0x00 0x07 */
        {
            const uint8_t b[4] = { 0x40, 0x02, 0x00, 0x07 };
            CHECK(mqtt_parse(b, sizeof(b), &v, &used) == MQTT_OK);
            CHECK(mqtt_fsm_on_packet(&f, &v, T0, &out) == MQTT_OK);
        }
        /* 不支持的类型（QoS2 的 PUBREC 0x50）→ 如实上报，交给调用方决定 */
        {
            const uint8_t b[4] = { 0x50, 0x02, 0x00, 0x07 };
            CHECK(mqtt_parse(b, sizeof(b), &v, &used) == MQTT_OK);
            CHECK(mqtt_fsm_on_packet(&f, &v, T0, &out) == MQTT_ERR_UNSUPPORTED);
        }
    }

    /* ---- 9. 背压：下行入队失败 → 不回 PUBACK（让 broker 重投） ---- */
    {
        mqtt_fsm_t f;
        mqtt_inflight_t q;
        mqtt_fsm_out_t out;
        mqtt_publish_rx_t rx;

        mqtt_inflight_init(&q);
        bring_up(&f, &q);
        memset(&rx, 0, sizeof(rx));
        rx.topic       = "safe/cmd";
        rx.payload     = (const uint8_t *)"{}";
        rx.payload_len = 2;
        rx.pkt_id      = 9;
        rx.qos         = 1;
        mqtt_fsm_on_event(&f, MQTT_EV_RECV_PUBLISH, &rx, T0, &out);
        CHECK(out.act == MQTT_ACT_DELIVER);

        /* 调用方入队失败 → 主动放弃 PUBACK */
        mqtt_fsm_cancel_puback(&f);
        mqtt_fsm_tick(&f, T0, &out);
        CHECK(out.act == MQTT_ACT_NONE);       /* ★ 不再产出 SEND_PUBACK */
        CHECK(f.pending_puback == 0);
    }

    /* ---- 10. NULL / 非法入参不崩 ---- */
    {
        mqtt_fsm_out_t out;
        mqtt_fsm_t f;
        mqtt_fsm_cfg_t cfg;
        mqtt_inflight_t q;

        mqtt_inflight_init(&q);
        mqtt_fsm_cfg_default(&cfg);

        mqtt_fsm_init(NULL, &cfg, &q);
        mqtt_fsm_init(&f, NULL, &q);           /* cfg 为 NULL → 用默认配置 */
        CHECK(f.cfg.keepalive_s == 20);
        mqtt_fsm_cfg_default(NULL);
        mqtt_fsm_init(&f, &cfg, NULL);         /* inflight 为 NULL → QoS0 场景可用 */

        mqtt_fsm_tick(NULL, T0, &out);
        mqtt_fsm_tick(&f, T0, NULL);
        mqtt_fsm_on_event(NULL, MQTT_EV_START, NULL, T0, &out);
        mqtt_fsm_on_event(&f, MQTT_EV_START, NULL, T0, NULL);
        CHECK(mqtt_fsm_on_packet(NULL, NULL, T0, &out) == MQTT_ERR_NULL);
        CHECK(mqtt_fsm_on_packet(&f, NULL, T0, &out) == MQTT_ERR_NULL);
        CHECK(mqtt_fsm_next_wake_ms(NULL, T0) == 0);
        CHECK(mqtt_fsm_publish_begin(NULL, NULL, 0, T0, &out) == 0);
        /* 未 CONNECTED 时 publish_begin 必须失败（不能把报文塞进 inflight 了事） */
        CHECK(mqtt_fsm_publish_begin(&f, (const uint8_t *)"x", 1u, T0, &out) == 0);
        mqtt_fsm_cancel_puback(NULL);
        CHECK(mqtt_fsm_state_name(MQTT_ST_IDLE) != NULL);
        CHECK(mqtt_fsm_state_name((mqtt_fsm_state_t)99) != NULL);
        CHECK(mqtt_fsm_action_name(MQTT_ACT_NONE) != NULL);
        CHECK(mqtt_fsm_action_name((mqtt_fsm_action_t)99) != NULL);
    }

    TEST_RESULT();
}
