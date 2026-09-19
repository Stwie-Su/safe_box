/**
 * @file mqtt_inflight.h
 * R9 自研 MQTT 客户端 T02：未确认报文队列（in-flight）
 *
 * 为什么需要：QoS1 的 PUBLISH 必须等 PUBACK；在等到之前，报文必须能被**重传**。
 * 重传的前提是把原始字节留下来 —— 所以本模块存的是「已组好的字节」，不是指针。
 *
 * ★ 硬约束（本项目踩过的坑）：**绝不存指向上层 payload 的指针**。
 *   rpc.c 在 publish 之后会立刻 cJSON_Delete(root)，指针必悬垂。
 *   这里把整条报文**拷贝**进固定缓冲，代价是几百字节，换来的是"重传时字节一定还在"。
 *
 * 与 codec 一样是纯逻辑层：不 malloc、不 IO、不取时间（时间由调用方注入）、不打印。
 * 全部用固定数组，适合单核 A7。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "core/remote/mqtt_codec.h"

/* 同时最多有多少条未确认报文。单核 A7 + 局域网场景，8 条足够，
 * 再多是内存浪费（每条都要留一份完整报文）。 */
#define MQTT_INFLIGHT_MAX 8

/* 单条报文的最大字节数。safe/log 的一条事件 JSON 通常在 200B 以内，
 * 给 512B 留足余量；超过则入队失败（调用方应记录并放弃该条，不要截断）。 */
#define MQTT_INFLIGHT_PKT_MAX 512

/* 最大重传次数。超过则丢弃该报文（不再重传），由上层决定是否重建。
 * MQTT 3.1.1 规定 DUP 只是"可能重复"的提示，接收方仍须幂等，
 * 因此无限重传毫无意义 —— 反倒会持续占用队列。 */
#define MQTT_INFLIGHT_MAX_RETRY 3

typedef struct {
    uint16_t pkt_id;
    uint8_t  pkt[MQTT_INFLIGHT_PKT_MAX];   /* ★ 已组好的字节（拷贝，非指针） */
    size_t   len;
    uint32_t sent_at_ms;                   /* 上次下发时刻（调用方时钟基） */
    uint8_t  retry;                        /* 已重传次数 */
    bool     in_use;
} mqtt_inflight_item_t;

typedef struct {
    mqtt_inflight_item_t items[MQTT_INFLIGHT_MAX];
    uint16_t next_id;                      /* 下一个要分配的 packet id */
} mqtt_inflight_t;

/* 清空队列。next_id 从 0 开始（首次 next_id() 会返回 1）。 */
void mqtt_inflight_init(mqtt_inflight_t *q);

/* 分配下一个 packet id：范围 1~65535，**跳过 0**（MQTT 3.1.1：0 是无效 id）。
 * 到 65535 后回绕到 1。 */
uint16_t mqtt_inflight_next_id(mqtt_inflight_t *q);

/**
 * 入队。pkt 为**已组好**的完整报文（含固定头），本函数拷一份。
 * 失败情形：队列满 → MQTT_ERR_NO_SPACE；报文过长 → MQTT_ERR_NO_SPACE；
 *          id 已在队列中 → MQTT_ERR_MALFORMED。
 */
int mqtt_inflight_add(mqtt_inflight_t *q, uint16_t id,
                      const uint8_t *pkt, size_t len, uint32_t now_ms);

/* 按 id 查找；未找到返回 NULL。 */
const mqtt_inflight_item_t * mqtt_inflight_find(const mqtt_inflight_t *q, uint16_t id);

/**
 * 收到 PUBACK：把该 id 从队列移除。
 * 返回 MQTT_OK = 确实确认了一条；MQTT_ERR_MALFORMED = 队列里没有这个 id
 * （重复 ACK 或迟到 ACK —— 上层应忽略，不要当成错误中断流程）。
 */
int mqtt_inflight_ack(mqtt_inflight_t *q, uint16_t id);

/**
 * 扫描一条「已超时、需要重传」的报文。
 * 找到则：①把该报文的 **DUP 位置 1**（MQTT 3.1.1 §3.3.1，仅对 PUBLISH 有意义，
 * 调用方需保证队列里放的是 PUBLISH）；②通过 out_* 返回其字节与长度；
 * ③**不更新** sent_at —— 调用方真正发出去后再调 mark_sent()。
 * 返回 MQTT_OK = 有一条要重传；MQTT_ERR_UNSUPPORTED = 没有（队列空或未超时）。
 */
int mqtt_inflight_timeout_scan(mqtt_inflight_t *q, uint32_t now_ms, uint32_t timeout_ms,
                               uint16_t *out_id, const uint8_t **out_pkt, size_t *out_len);

/**
 * 调用方真正把重传报文发出去之后调用：刷新 sent_at 并累加 retry。
 * 若 retry 已达 MQTT_INFLIGHT_MAX_RETRY，则**丢弃**该条（返回 MQTT_ERR_MALFORMED，
 * 调用方应记一条日志并放弃 —— 不再无限重传）。
 */
int mqtt_inflight_mark_sent(mqtt_inflight_t *q, uint16_t id, uint32_t now_ms);

size_t mqtt_inflight_count(const mqtt_inflight_t *q);
