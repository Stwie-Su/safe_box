/**
 * @file mqtt_fsm.h
 * MQTT 连接状态机：决定「什么时候连、什么时候重连、什么时候重传、什么时候发心跳」。
 *
 * 这一层只做判断，不做真正的网络读写：
 *   - 不建 socket、不起线程、不取时间、不申请内存、不打印；
 *   - 时间由调用方传进来（now_ms），随机种子也在配置里给，
 *     所以同一串输入一定得到同一串输出，测试时不用真 broker。
 *   - 它「说」要做什么（MQTT_ACT_*），由网络线程去执行。
 *
 * 状态流转：
 *   IDLE → CONNECTING → CONNECTED →（断线）→ WAIT_RETRY → CONNECTING → ...
 *   停止时走 DISCONNECTING。
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

/* 不做「已订阅」这个状态：发完 SUBSCRIBE 就可以收发了，
 * 用 pending_subscribe 标志表示「连上后还要补发订阅」就够了。 */
typedef enum {
    MQTT_ST_IDLE = 0,       /* 没启动 / 已停止 */
    MQTT_ST_CONNECTING,     /* 正在连，还没收到 CONNACK */
    MQTT_ST_CONNECTED,      /* 连上了，可以收发 */
    MQTT_ST_WAIT_RETRY,     /* 断了，等退避时间到了再连 */
    MQTT_ST_DISCONNECTING,  /* 已发 DISCONNECT，等连接关闭 */
} mqtt_fsm_state_t;

/* 驱动状态机的事件。 */
typedef enum {
    MQTT_EV_START,           /* 上层要求启动 */
    MQTT_EV_STOP,            /* 上层要求停止 */
    MQTT_EV_SOCK_CONNECTED,  /* 连接建立成功 */
    MQTT_EV_SOCK_FAILED,     /* 连接失败 */
    MQTT_EV_IO_ERROR,        /* 读写失败或对方关闭 */
    MQTT_EV_RECV_CONNACK,    /* arg: const mqtt_connack_t * */
    MQTT_EV_RECV_SUBACK,     /* arg: const mqtt_suback_t * */
    MQTT_EV_RECV_PUBLISH,    /* arg: const mqtt_publish_rx_t * */
    MQTT_EV_RECV_PUBACK,     /* arg: const uint16_t *（报文 id） */
    MQTT_EV_RECV_PINGRESP,   /* 无参数 */
    MQTT_EV_TX_FLUSHED,      /* 一次发送完成 */
} mqtt_fsm_ev_t;

/* 事件带的参数（只在调用期间有效，状态机不保存指针）。 */
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
    const uint8_t * payload;   /* 指向调用方缓冲区，不拷贝 */
    size_t   payload_len;
    uint16_t pkt_id;
    bool     dup;
    uint8_t  qos;
    bool     retain;
} mqtt_publish_rx_t;

/* 配置：初始化时给一次，之后不变（测试才能复现）。 */
typedef struct {
    uint16_t keepalive_s;        /* 心跳间隔，建议 20 秒 */
    uint32_t ping_timeout_ms;    /* 发心跳后等回应的时间，一般 = keepalive * 1000 */
    uint32_t retry_timeout_ms;   /* QoS1 消息多久没确认就重传，建议 5000 */
    uint32_t backoff_base_ms;    /* 重连等待起点，建议 500 */
    uint32_t backoff_max_ms;     /* 重连等待上限，建议 30000 */
    /* 发订阅后等确认的时间，建议 5000。
     * 没有这个超时的话，broker 不回 SUBACK 时设备会一直「显示在线但收不到指令」，
     * 而且永远不恢复。超时后当断线处理，重连一次。 */
    uint32_t suback_timeout_ms;
    uint32_t rng_seed;           /* 随机种子（测试里固定住，结果才可复现） */
} mqtt_fsm_cfg_t;

/* 状态机要执行的动作。 */
typedef enum {
    MQTT_ACT_NONE = 0,
    MQTT_ACT_OPEN_SOCKET,      /* 发起连接 */
    MQTT_ACT_CLOSE_SOCKET,
    MQTT_ACT_SEND_CONNECT,     /* 发 CONNECT（含遗嘱） */
    MQTT_ACT_SEND_SUBSCRIBE,
    MQTT_ACT_SEND_PINGREQ,
    MQTT_ACT_SEND_DISCONNECT,
    MQTT_ACT_SEND_PUBLISH,     /* 发新消息；id 已分配好 */
    MQTT_ACT_SEND_PUBACK,      /* 确认收到下行消息；带 id */
    MQTT_ACT_RETRANSMIT,       /* 重传；带 id */
    MQTT_ACT_DROP_INFLIGHT,    /* 重传次数用完，丢掉并计数 */
    MQTT_ACT_DELIVER,          /* 把下行消息交给上层 */
    MQTT_ACT_NOTIFY_STATE,     /* 连接状态变了，通知上层 / 界面 */
} mqtt_fsm_action_t;

typedef struct {
    mqtt_fsm_action_t act;
    uint16_t pkt_id;
    uint32_t next_wake_ms;     /* 建议下次多久后再来（poll 超时用） */
    int      connack_code;     /* NOTIFY_STATE 时有意义 */
    bool     state_changed;
} mqtt_fsm_out_t;

/* 状态机本体。字段都能读，测试会直接断言。 */
typedef struct {
    mqtt_fsm_state_t state;
    /* 定时器 */
    uint32_t last_tx_ms;     /* 最后一次发送的时刻（判断该不该发心跳） */
    uint32_t ping_sent_ms;   /* 心跳发出时刻；0 = 没发 */
    uint32_t sub_sent_ms;    /* 订阅发出时刻；0 = 没发 */
    uint32_t retry_at_ms;    /* 什么时候可以重连 */
    /* 退避 */
    uint32_t backoff_ms;     /* 当前等待时长 */
    uint8_t  retry_count;    /* 连续失败次数 */
    /* 连续订阅失败次数。
     * 单独计数：连上成功会把 retry_count 清零，而订阅失败是在连上之后才发生的，
     * 只用一个计数器会导致每次重连都从最小值开始，变成固定频率疯狂重连。 */
    uint8_t  suback_reject_streak;
    uint32_t rng;            /* 自己实现的伪随机状态（不用 rand()） */
    /* 标志 */
    bool     pending_ping;
    bool     pending_subscribe;
    bool     pending_suback;  /* 已发订阅，等确认 */
    bool     session_present;
    uint8_t  connack_code;
    /* broker 实际授予的 QoS（0/1/2；0xFF = 还没收到过）。
     * 单独存是为了能发现「申请了 QoS1，实际只给 0」——否则排障时完全看不见。 */
    uint8_t  granted_qos;
    /* 关联对象，由网络线程注入；QoS0 场景可以是 NULL */
    mqtt_inflight_t * inflight;
    /* 统计 */
    uint32_t stat_connects, stat_reconnects, stat_ping_timeout,
             stat_retransmit, stat_dropped, stat_rx_publish;
    uint32_t stat_suback_reject;   /* broker 明确拒绝订阅的次数 */
    uint32_t stat_suback_timeout;  /* 订阅压根没回应的次数（和被拒是两回事） */
    mqtt_fsm_cfg_t cfg;

    /* 下面几个是实现细节，测试也可以看：
     * pending_puback 是「收到了但还没回确认」的 id（0 = 没有）。
     * 只有成功交给上层才回确认；队列满时故意不回，让 broker 重发。
     * acts 是动作缓冲：一次事件可能产生好几个动作，最多 4 个。 */
    uint16_t        pending_puback;
    mqtt_fsm_out_t  acts[4];
    uint8_t         act_head;
    uint8_t         act_n;
} mqtt_fsm_t;

/* 初始化。q 可以是 NULL（只发 QoS0 时不需要未确认队列）。 */
void mqtt_fsm_init(mqtt_fsm_t *f, const mqtt_fsm_cfg_t *cfg, mqtt_inflight_t *q);

/* 填一份推荐的默认配置（心跳 20s，重连 0.5s 起、最多 30s）。 */
void mqtt_fsm_cfg_default(mqtt_fsm_cfg_t *cfg);

/* 时间推进，每次 poll 都调。同一拍可能产生多个动作，
 * 用法是 while (out.act != MQTT_ACT_NONE) 一直取到没有为止。
 * 它自己也会推进定时器，比如心跳超时就直接转成「等重连」。 */
void mqtt_fsm_tick(mqtt_fsm_t *f, uint32_t now_ms, mqtt_fsm_out_t *out);

/* 喂一个事件。arg 按事件类型强转，没有参数的事件传 NULL。 */
void mqtt_fsm_on_event(mqtt_fsm_t *f, mqtt_fsm_ev_t ev, const void *arg,
                       uint32_t now_ms, mqtt_fsm_out_t *out);

/* 收到一个报文时的统一入口（内部按类型分派）。返回 MQTT_OK 或负的错误码。 */
int  mqtt_fsm_on_packet(mqtt_fsm_t *f, const mqtt_pkt_view_t *v,
                        uint32_t now_ms, mqtt_fsm_out_t *out);

/* 距离下一个定时器到期还有多少毫秒（给 poll 当超时用）。0 = 现在就该处理。 */
uint32_t mqtt_fsm_next_wake_ms(const mqtt_fsm_t *f, uint32_t now_ms);

/* 准备发一条 QoS1 消息：分配 id、存进未确认队列，返回 id（0 = 失败）。
 * pkt/len 是已经组好的完整报文；成功时 out 里是 MQTT_ACT_SEND_PUBLISH。 */
uint16_t mqtt_fsm_publish_begin(mqtt_fsm_t *f, const uint8_t *pkt, size_t len,
                                uint32_t now_ms, mqtt_fsm_out_t *out);

/* 放弃这条待回的确认。效果是 broker 之后会重发（DUP=1），
 * 慢一点，但不会丢指令。 */
void mqtt_fsm_cancel_puback(mqtt_fsm_t *f);

/* 状态名 / 动作名，打日志用。 */
const char * mqtt_fsm_state_name(mqtt_fsm_state_t s);
const char * mqtt_fsm_action_name(mqtt_fsm_action_t a);

#ifdef __cplusplus
} /*extern "C"*/
#endif
