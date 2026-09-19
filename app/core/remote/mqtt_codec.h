/**
 * @file mqtt_codec.h
 * MQTT 3.1.1 报文编解码层（R9 自研客户端 · T01）
 *
 * 设计约束（这是整个 R9 的根）：
 *   **纯函数层** —— 不 malloc、不做 IO、不取时间、不打印、不碰 socket/线程。
 *   因此重传时序、退避曲线、半包/粘包全部可以在 PC 上做确定性单测。
 *
 * 解析出的 payload 指针指向**调用方缓冲区内部**，本模块不分配、不拷贝。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MQTT 3.1.1 §2.2.1 报文类型 */
typedef enum {
    MQTT_PKT_CONNECT     = 0x01,
    MQTT_PKT_CONNACK     = 0x02,
    MQTT_PKT_PUBLISH     = 0x03,
    MQTT_PKT_PUBACK      = 0x04,
    MQTT_PKT_PUBREC      = 0x05,
    MQTT_PKT_PUBREL      = 0x06,
    MQTT_PKT_PUBCOMP     = 0x07,
    MQTT_PKT_SUBSCRIBE   = 0x08,
    MQTT_PKT_SUBACK      = 0x09,
    MQTT_PKT_UNSUBSCRIBE = 0x0A,
    MQTT_PKT_UNSUBACK    = 0x0B,
    MQTT_PKT_PINGREQ     = 0x0C,
    MQTT_PKT_PINGRESP    = 0x0D,
    MQTT_PKT_DISCONNECT  = 0x0E,
} mqtt_pkt_type_t;

/* §2.2.3：剩余长度是变长整数，最多 4 字节，最大值 268435455 (0x0FFFFFFF) */
#define MQTT_REMAINING_MAX 268435455u

/* CONNACK 返回码（§3.2.2.3） */
#define MQTT_CONNACK_ACCEPTED 0

typedef enum {
    MQTT_OK               =   0,
    MQTT_ERR_NULL         =  -1,   /* 空指针 */
    MQTT_ERR_NO_SPACE     =  -2,   /* 输出缓冲不足 */
    MQTT_ERR_MALFORMED    =  -3,   /* 结构非法（如变长整数第 4 字节仍带续位） */
    MQTT_ERR_TRUNCATED    =  -4,   /* 数据不足一个完整报文（半包，应继续收） */
    MQTT_ERR_UNSUPPORTED  =  -5,   /* 本客户端不支持的特性（如 QoS2） */
} mqtt_err_t;

const char * mqtt_err_name(mqtt_err_t e);

/* ---------- 变长剩余长度（§2.2.3） ---------- */

/**
 * 编码。out_n 返回写入字节数（1~4）。
 * len > MQTT_REMAINING_MAX → MQTT_ERR_MALFORMED。
 */
int mqtt_remaining_encode(uint32_t len, uint8_t *out, size_t cap, size_t *out_n);

/**
 * 解码。out_n 返回消耗的字节数（1~4）。
 * 第 4 字节仍带续位 → MQTT_ERR_MALFORMED（协议不允许，防无限延长）。
 * 数据不足以读完变长整数 → MQTT_ERR_TRUNCATED。
 */
int mqtt_remaining_decode(const uint8_t *in, size_t in_n, uint32_t *out_len, size_t *out_n);

/* ---------- 组包（纯拼字节，返回写入长度） ---------- */

int mqtt_pack_connect(uint8_t *buf, size_t cap, size_t *out_n,
                      const char *client_id, uint16_t keepalive_s, bool clean_session,
                      const char *will_topic, const char *will_msg,
                      const char *username, const char *password);

int mqtt_pack_publish(uint8_t *buf, size_t cap, size_t *out_n,
                      const char *topic, const uint8_t *payload, size_t payload_len,
                      uint16_t pkt_id, bool dup, uint8_t qos, bool retain);

int mqtt_pack_puback(uint8_t *buf, size_t cap, size_t *out_n, uint16_t pkt_id);

int mqtt_pack_subscribe(uint8_t *buf, size_t cap, size_t *out_n,
                        const char *topic, uint8_t qos, uint16_t pkt_id);

int mqtt_pack_pingreq(uint8_t *buf, size_t cap, size_t *out_n);

int mqtt_pack_disconnect(uint8_t *buf, size_t cap, size_t *out_n);

/* ---------- 解析 ---------- */

/**
 * 已解析出的一个报文的**视图**。
 * payload 指向 buf 内部（不拷贝）；total_len 为本报文总长度（含固定头）。
 */
typedef struct {
    mqtt_pkt_type_t type;
    uint8_t         flags;
    uint32_t        remaining;
    const uint8_t * payload;      /* 剩余长度之后的部分（可变头 + 载荷） */
    size_t          payload_len;
    size_t          total_len;
} mqtt_pkt_view_t;

/**
 * 从缓冲区头部解析一个报文。
 * 半包（数据不足）→ MQTT_ERR_TRUNCATED，*consumed = 0（调用方应继续收字节后重试）。
 * 成功 → *consumed = 本报文总长度（用于粘包时前进游标）。
 */
int mqtt_parse(const uint8_t *buf, size_t len, mqtt_pkt_view_t *out, size_t *consumed);

int mqtt_parse_connack(const mqtt_pkt_view_t *v, bool *session_present, uint8_t *ret_code);

/**
 * topic 会**补 '\0'**（topic_cap 需包含结尾空间）。
 * QoS0 时 pkt_id 输出 0（MQTT 3.1.1：QoS0 的 PUBLISH 不带 packet identifier）。
 */
int mqtt_parse_publish(const mqtt_pkt_view_t *v, char *topic, size_t topic_cap,
                       const uint8_t **payload, size_t *payload_len,
                       uint16_t *pkt_id, bool *dup, uint8_t *qos, bool *retain);

int mqtt_parse_puback(const mqtt_pkt_view_t *v, uint16_t *pkt_id);
int mqtt_parse_suback(const mqtt_pkt_view_t *v, uint16_t *pkt_id, uint8_t *ret_code);
