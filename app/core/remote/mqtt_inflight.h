/**
 * @file mqtt_inflight.h
 * 「已发出、还没收到确认」的消息队列 —— QoS1 重传就靠它。
 *
 * 为什么存整条报文的字节，而不是存指针：上层发完消息就把 JSON 释放了，
 * 指针会变成野指针；这里拷一份字节，重传时一定还在。
 *
 * 纯逻辑层：不申请内存、不做 IO、不取时间（时间由调用方传）。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "core/remote/mqtt_codec.h"

/* 最多同时有几条等确认。局域网场景 8 条足够，每条都要占一份报文空间。 */
#define MQTT_INFLIGHT_MAX 8

/* 单条报文最大字节数。一条事件 JSON 通常 200 字节以内，512 留足余量；
 * 超过就入队失败（由上层记录并放弃，不要截断）。 */
#define MQTT_INFLIGHT_PKT_MAX 512

/* 最多重传几次。超过就丢掉，不再无限重传。 */
#define MQTT_INFLIGHT_MAX_RETRY 3

typedef struct {
    uint16_t pkt_id;
    uint8_t  pkt[MQTT_INFLIGHT_PKT_MAX];   /* 已组好的完整报文（拷贝） */
    size_t   len;
    uint32_t sent_at_ms;                   /* 上次发出时刻 */
    uint8_t  retry;                        /* 已经重传几次 */
    bool     in_use;
} mqtt_inflight_item_t;

typedef struct {
    mqtt_inflight_item_t items[MQTT_INFLIGHT_MAX];
    uint16_t next_id;                      /* 下一个要分配的消息编号 */
} mqtt_inflight_t;

/* 清空队列。 */
void mqtt_inflight_init(mqtt_inflight_t *q);

/* 分配下一个消息编号：1~65535，跳过 0（协议规定 0 无效），到顶后回到 1。 */
uint16_t mqtt_inflight_next_id(mqtt_inflight_t *q);

/* 入队（会拷贝一份报文）。队列满 / 报文太长 → MQTT_ERR_NO_SPACE；
 * id 已经在队列里 → MQTT_ERR_MALFORMED。 */
int mqtt_inflight_add(mqtt_inflight_t *q, uint16_t id,
                      const uint8_t *pkt, size_t len, uint32_t now_ms);

/* 按 id 查找，找不到返回 NULL。 */
const mqtt_inflight_item_t * mqtt_inflight_find(const mqtt_inflight_t *q, uint16_t id);

/* 收到确认：把这条从队列移除。
 * 返回 MQTT_OK = 确实删了一条；MQTT_ERR_MALFORMED = 队列里没这个 id
 * （重复的或迟到的确认，忽略即可，不要当错误处理）。 */
int mqtt_inflight_ack(mqtt_inflight_t *q, uint16_t id);

/* 找一条「超时该重传」的消息：把 DUP 标志置 1，返回它的字节和长度。
 * 注意：这里不改发送时间，真要发出去之后由调用方调 mark_sent()。
 * 返回 MQTT_OK = 找到一条；MQTT_ERR_UNSUPPORTED = 没有（队列空或都还没超时）。 */
int mqtt_inflight_timeout_scan(mqtt_inflight_t *q, uint32_t now_ms, uint32_t timeout_ms,
                               uint16_t *out_id, const uint8_t **out_pkt, size_t *out_len);

/* 真发出去之后调：刷新发送时间、重传次数 +1。
 * 如果重传次数已经到上限，这条会被丢掉并返回 MQTT_ERR_MALFORMED（调用方记日志放弃）。 */
int mqtt_inflight_mark_sent(mqtt_inflight_t *q, uint16_t id, uint32_t now_ms);

/* 当前队列里有几条。 */
size_t mqtt_inflight_count(const mqtt_inflight_t *q);
