/**
 * @file mqtt_codec.h
 * MQTT 3.1.1 报文的组包与解析：把结构体拼成字节、把字节拆回结构体。
 *
 * 这一层是纯函数：不申请内存、不做网络读写、不取时间、不打印，
 * 所以「半包、粘包、畸形报文」这些情况都能在电脑上用固定数据测出来。
 *
 * 解析出来的 payload 指针指向**调用方传进来的缓冲区内部**，本层不拷贝、不释放。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 报文类型（MQTT 3.1.1 第 2.2.1 节）。 */
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

/* 剩余长度是变长整数，最多 4 个字节，最大 268435455。 */
#define MQTT_REMAINING_MAX 268435455u

/* CONNACK 的「接受」返回码。 */
#define MQTT_CONNACK_ACCEPTED 0

typedef enum {
    MQTT_OK               =   0,
    MQTT_ERR_NULL         =  -1,   /* 空指针 */
    MQTT_ERR_NO_SPACE     =  -2,   /* 输出缓冲不够 */
    MQTT_ERR_MALFORMED    =  -3,   /* 报文结构非法 */
    MQTT_ERR_TRUNCATED    =  -4,   /* 数据还不完整（半包，继续收） */
    MQTT_ERR_UNSUPPORTED  =  -5,   /* 不支持的特性（比如 QoS2） */
} mqtt_err_t;

/* 错误码转字符串，打日志用。 */
const char * mqtt_err_name(mqtt_err_t e);

/* ---------- 剩余长度的编解码 ---------- */

/* 编码：out_n 返回写了几个字节（1~4）。len 太大返回 MQTT_ERR_MALFORMED。 */
int mqtt_remaining_encode(uint32_t len, uint8_t *out, size_t cap, size_t *out_n);

/* 解码：out_n 返回读了几个字节。第 4 字节还带续位 = 非法；数据不够 = TRUNCATED。 */
int mqtt_remaining_decode(const uint8_t *in, size_t in_n, uint32_t *out_len, size_t *out_n);

/* ---------- 组包（拼好字节，返回写入长度） ---------- */

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

/* 解析出来的一个报文「视图」。payload 指向 buf 内部，不拷贝；
 * total_len 是这条报文一共多长（含固定头），粘包时用它往前挪。 */
typedef struct {
    mqtt_pkt_type_t type;
    uint8_t         flags;
    uint32_t        remaining;
    const uint8_t * payload;      /* 剩余长度之后的部分 */
    size_t          payload_len;
    size_t          total_len;
} mqtt_pkt_view_t;

/* 从缓冲区开头解析一个报文。
 * 数据不够（半包）返回 MQTT_ERR_TRUNCATED，*consumed = 0，调用方继续收；
 * 成功时 *consumed = 这条报文的总长度。 */
int mqtt_parse(const uint8_t *buf, size_t len, mqtt_pkt_view_t *out, size_t *consumed);

int mqtt_parse_connack(const mqtt_pkt_view_t *v, bool *session_present, uint8_t *ret_code);

/* 解析 PUBLISH。topic 会自动补 '\0'（topic_cap 要含结尾空间）。
 * QoS0 没有 packet id，pkt_id 输出 0。 */
int mqtt_parse_publish(const mqtt_pkt_view_t *v, char *topic, size_t topic_cap,
                       const uint8_t **payload, size_t *payload_len,
                       uint16_t *pkt_id, bool *dup, uint8_t *qos, bool *retain);

int mqtt_parse_puback(const mqtt_pkt_view_t *v, uint16_t *pkt_id);
int mqtt_parse_suback(const mqtt_pkt_view_t *v, uint16_t *pkt_id, uint8_t *ret_code);
