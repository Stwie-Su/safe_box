/**
 * @file mqtt_fsm.h
 * R9 自研 MQTT 客户端 T03：连接/重传/重连状态机（**纯逻辑层**）。
 *
 * ★ 本模块最重要的设计决定（这是它能被确定性单测的前提）：
 *   **不建 SOCKET、不起线程、不取时间、不申请堆内存、不打印。**
 *   时间一律由 `now_ms` 参数注入，随机数由 `cfg.rng_seed` 注入。
 *   于是「喂这些字节 + 这些时刻 → 产出这些动作」可以在 PC 上精确断言，
 *   不需要真 broker、不需要 sleep、不需要碰运气。
 *
 * ★ 为什么「输出动作」而不是回调：
 *   回调会把 IO 语义绑进纯逻辑层，单测就得造 SOCKET。输出动作让调用方
 *   （T04 的 net 线程）负责执行，本层只负责「说该做什么」。
 *
 * 硬约束（tools/check_layers.sh 与 R9 验收判据）：
 *   mqtt_fsm.c 内不得出现 hal_time / socket / pthread / malloc / printf 等字样。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/remote/mqtt_codec.h"
#include "core/remote/mqtt_inflight.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 状态集（5 个，刻意不做「已订阅」态） ----------------
 * 为什么不做「已订阅」态：MQTT 3.1.1 允许 SUBSCRIBE 后立刻 PUBLISH，
 * 且 broker 不会在 SUBACK 之前推下行。做成状态会多两个迁移、两种失败路径，
 * 收益为零。改用 `pending_subscribe` 标志表达「CONNACK 后需要发 SUBSCRIBE」
 * —— clean_session=1 时重连必须重发，这个标志就是它的载体。 */
typedef enum {
    MQTT_ST_IDLE = 0,       /* 未启动 / 已停止 */
    MQTT_ST_CONNECTING,     /* 已发起非阻塞连接，尚未收到 CONNACK */
    MQTT_ST_CONNECTED,      /* 已收 CONNACK(0)，可收发 */
    MQTT_ST_WAIT_RETRY,     /* 连接失败 / 链路断开，等退避到期后重连 */
    MQTT_ST_DISCONNECTING,  /* 已发 DISCONNECT（优雅停止），等链路关闭 */
} mqtt_fsm_state_t;

/* ---------------- 事件集 ---------------- */
typedef enum {
    MQTT_EV_START,           /* 上层要求启动              */
    MQTT_EV_STOP,            /* 上层要求停止              */
    MQTT_EV_SOCK_CONNECTED,  /* 非阻塞连接完成且成功      */
    MQTT_EV_SOCK_FAILED,     /* 连接失败                  */
    MQTT_EV_IO_ERROR,        /* 读/写失败 或 对端 EOF     */
    MQTT_EV_RECV_CONNACK,    /* arg: const mqtt_connack_t *      */
    MQTT_EV_RECV_SUBACK,     /* arg: const mqtt_suback_t *       */
    MQTT_EV_RECV_PUBLISH,    /* arg: const mqtt_publish_rx_t *   */
    MQTT_EV_RECV_PUBACK,     /* arg: const uint16_t *（pkt_id）  */
    MQTT_EV_RECV_PINGRESP,   /* 无 arg                    */
    MQTT_EV_TX_FLUSHED,      /* 一次下发完成（可更新 last_tx_ms） */
} mqtt_fsm_ev_t;

/* 事件附带参数（本层**不持有**这些指针，仅在调用期间有效） */
typedef struct {
    bool     session_present;
    uint8_t  ret_code;
} mqtt_connack_t;

typedef struct {
    uint16_t pkt_id;
    uint8_t  ret_code;
} mqtt_suback_t;

typedef struct {
    const char * topic;
    const uint8_t * payload;   /* 指向调用方缓冲内部，不拷贝 */
    size_t   payload_len;
    uint16_t pkt_id;
    bool     dup;
    uint8_t  qos;
    bool     retain;
} mqtt_publish_rx_t;

/* ---------------- 配置（init 时注入，运行期不变 → 单测可固定） ---------------- */
typedef struct {
    uint16_t keepalive_s;        /* MQTT keepalive，建议 20 */
    uint32_t ping_timeout_ms;    /* PINGREQ 后等 PINGRESP 的超时，建议 keepalive*1000 */
    uint32_t retry_timeout_ms;   /* QoS1 未确认重传超时，建议 5000 */
    uint32_t backoff_base_ms;    /* 退避基数，建议 500 */
    uint32_t backoff_max_ms;     /* 退避封顶，建议 30000 */
    uint32_t rng_seed;           /* 抖动 PRNG 种子（单测固定 → 结果确定） */
} mqtt_fsm_cfg_t;

/* ---------------- 输出动作：状态机「说」要做什么，不自己做 IO ---------------- */
typedef enum {
    MQTT_ACT_NONE = 0,
    MQTT_ACT_OPEN_SOCKET,      /* 要求传输层发起非阻塞连接      */
    MQTT_ACT_CLOSE_SOCKET,
    MQTT_ACT_SEND_CONNECT,     /* 含 LWT                        */
    MQTT_ACT_SEND_SUBSCRIBE,
    MQTT_ACT_SEND_PINGREQ,
    MQTT_ACT_SEND_DISCONNECT,
    MQTT_ACT_SEND_PUBLISH,     /* 新发；pkt_id 已由 inflight 分配 */
    MQTT_ACT_SEND_PUBACK,      /* 下行 QoS1 确认；pkt_id         */
    MQTT_ACT_RETRANSMIT,       /* 重传；pkt_id                   */
    MQTT_ACT_DROP_INFLIGHT,    /* 重传次数用尽，丢弃并记 stat    */
    MQTT_ACT_DELIVER,          /* 下行消息入队（交 rpc 层）      */
    MQTT_ACT_NOTIFY_STATE,     /* 连接状态变化，通知上层/UI      */
} mqtt_fsm_action_t;

typedef struct {
    mqtt_fsm_action_t act;
    uint16_t pkt_id;
    uint32_t next_wake_ms;     /* 建议的下次唤醒间隔（poll 超时上界） */
    int      connack_code;     /* NOTIFY_STATE 时有效 */
    bool     state_changed;
} mqtt_fsm_out_t;

/* ---------------- 状态机实体 ---------------- */
typedef struct {
    mqtt_fsm_state_t state;
    /* ---- 定时器 ---- */
    uint32_t last_tx_ms;     /* 最后一次**发送**报文的时刻（keepalive 判据） */
    uint32_t ping_sent_ms;   /* PINGREQ 发出时刻；0 = 未发 */
    uint32_t retry_at_ms;    /* WAIT_RETRY 的到期时刻 */
    /* ---- 退避 ---- */
    uint32_t backoff_ms;     /* 当前退避时长 */
    uint8_t  retry_count;    /* 连续失败次数 */
    uint32_t rng;            /* xorshift32 私有状态（★ 不用 rand()） */
    /* ---- 标志 ---- */
    bool     pending_ping;
    bool     pending_subscribe;
    bool     session_present;
    uint8_t  connack_code;
    /* ---- 关联（不持有，由 T04 注入实体） ---- */
    mqtt_inflight_t * inflight;  /* T02 的未确认队列；可为 NULL（QoS0 场景） */
    /* ---- 统计（供 mqtt_stats() 与单测断言） ---- */
    uint32_t stat_connects, stat_reconnects, stat_ping_timeout,
             stat_retransmit, stat_dropped, stat_rx_publish;
    mqtt_fsm_cfg_t cfg;

    /* ---- 以下为实现细节（单测不直接读，但可见以便断言） ----
     * pending_puback：下行 QoS1 已收到、待回 PUBACK 的 pkt_id（0 = 无）。
     *   设计取舍：入队成功才回 PUBACK；入队失败（下行队列满）由调用方
     *   调 mqtt_fsm_cancel_puback() 主动放弃 —— 不发 PUBACK 让 broker 重投，
     *   绝不能静默丢指令（见设计文档 §1.2(d)）。
     * acts/act_head/act_n：一次事件可能产出多个动作（如 CONNACK(0) 同时产出
     *   SEND_SUBSCRIBE 与 NOTIFY_STATE），这里放一个容量 4 的定长环形 FIFO。 */
    uint16_t        pending_puback;
    mqtt_fsm_out_t  acts[4];
    uint8_t         act_head;
    uint8_t         act_n;
} mqtt_fsm_t;

/* ---------------- 接口 ---------------- */

void mqtt_fsm_init(mqtt_fsm_t *f, const mqtt_fsm_cfg_t *cfg, mqtt_inflight_t *q);

/* 填一份推荐配置（keepalive 20s / 退避 500ms 起步、封顶 30s）。
 * 设计文档签名表未列此函数，但 T04 与单测都需要一份默认值，放这里避免两处各写常数。 */
void mqtt_fsm_cfg_default(mqtt_fsm_cfg_t *cfg);

/**
 * 时间推进（每拍 / 每次 poll 返回都调）。now_ms 由调用方注入。
 * 同一拍可能产出多个动作：调用方需 `while (out.act != MQTT_ACT_NONE)` 循环消费
 * （每次消费后再调一次本函数即可取出下一个）。
 * ★ 本函数内部会推进定时器并可能直接完成状态迁移（如 PING 超时 → WAIT_RETRY）。
 */
void mqtt_fsm_tick(mqtt_fsm_t *f, uint32_t now_ms, mqtt_fsm_out_t *out);

/* 事件驱动。arg 按事件类型强转；无 arg 的事件传 NULL。 */
void mqtt_fsm_on_event(mqtt_fsm_t *f, mqtt_fsm_ev_t ev, const void *arg,
                       uint32_t now_ms, mqtt_fsm_out_t *out);

/* 收包统一入口（内部按 type 分派到对应 EV_*）。返回 MQTT_OK 或负的错误码。 */
int  mqtt_fsm_on_packet(mqtt_fsm_t *f, const mqtt_pkt_view_t *v,
                        uint32_t now_ms, mqtt_fsm_out_t *out);

/* 轮询：距离下一个定时器到期还有多少 ms（供 poll 超时用）。返回 0 = 现在就该动。 */
uint32_t mqtt_fsm_next_wake_ms(const mqtt_fsm_t *f, uint32_t now_ms);

/**
 * 请求发送一条 QoS1 上行：分配 pkt_id + 入 inflight，返回 pkt_id（0 = 失败）。
 * pkt/len 为**已组好**的字节（由 mqtt_pack_publish 产出）。
 * 返回非 0 时，out 里是 MQTT_ACT_SEND_PUBLISH（失败时为 MQTT_ACT_NONE）。
 */
uint16_t mqtt_fsm_publish_begin(mqtt_fsm_t *f, const uint8_t *pkt, size_t len,
                                uint32_t now_ms, mqtt_fsm_out_t *out);

/**
 * 放弃一条待发的 PUBACK（下行队列满时调用）。
 * 语义：不回 PUBACK → broker 判定 QoS1 未确认 → 重投（DUP=1）。
 * 代价是处理变慢，收益是**绝不静默丢指令**（安全设备的红线）。
 */
void mqtt_fsm_cancel_puback(mqtt_fsm_t *f);

const char * mqtt_fsm_state_name(mqtt_fsm_state_t s);
const char * mqtt_fsm_action_name(mqtt_fsm_action_t a);

#ifdef __cplusplus
} /*extern "C"*/
#endif
