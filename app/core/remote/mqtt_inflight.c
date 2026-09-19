#include "core/remote/mqtt_inflight.h"
#include <string.h>

void mqtt_inflight_init(mqtt_inflight_t *q)
{
    if (!q) return;
    memset(q, 0, sizeof(*q));
    q->next_id = 0;
}

uint16_t mqtt_inflight_next_id(mqtt_inflight_t *q)
{
    if (!q) return 0;
    if (q->next_id >= 0xFFFF) q->next_id = 0;     /* 回绕：65535 -> 1 */
    q->next_id++;
    if (q->next_id == 0) q->next_id = 1;          /* 0 是无效 id，永不分配 */
    return q->next_id;
}

int mqtt_inflight_add(mqtt_inflight_t *q, uint16_t id,
                      const uint8_t *pkt, size_t len, uint32_t now_ms)
{
    if (!q || !pkt) return MQTT_ERR_NULL;
    if (id == 0) return MQTT_ERR_MALFORMED;        /* 0 不是合法 packet id */
    if (len == 0 || len > MQTT_INFLIGHT_PKT_MAX) return MQTT_ERR_NO_SPACE;

    /* 同一 id 不允许重复入队 —— 否则 ACK 时无法区分是哪一条 */
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        if (q->items[i].in_use && q->items[i].pkt_id == id) return MQTT_ERR_MALFORMED;
    }

    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        if (q->items[i].in_use) continue;
        q->items[i].pkt_id     = id;
        memcpy(q->items[i].pkt, pkt, len);         /* ★ 拷字节，不存指针 */
        q->items[i].len        = len;
        q->items[i].sent_at_ms = now_ms;
        q->items[i].retry      = 0;
        q->items[i].in_use     = true;
        return MQTT_OK;
    }
    return MQTT_ERR_NO_SPACE;                      /* 队列满：拒绝新入队 */
}

const mqtt_inflight_item_t * mqtt_inflight_find(const mqtt_inflight_t *q, uint16_t id)
{
    if (!q || id == 0) return NULL;
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        if (q->items[i].in_use && q->items[i].pkt_id == id) return &q->items[i];
    }
    return NULL;
}

int mqtt_inflight_ack(mqtt_inflight_t *q, uint16_t id)
{
    if (!q || id == 0) return MQTT_ERR_NULL;
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        if (q->items[i].in_use && q->items[i].pkt_id == id) {
            q->items[i].in_use = false;
            q->items[i].len    = 0;
            return MQTT_OK;
        }
    }
    /* 迟到的 / 重复的 PUBACK：不是错误，忽略即可 */
    return MQTT_ERR_MALFORMED;
}

int mqtt_inflight_timeout_scan(mqtt_inflight_t *q, uint32_t now_ms, uint32_t timeout_ms,
                               uint16_t *out_id, const uint8_t **out_pkt, size_t *out_len)
{
    if (!q) return MQTT_ERR_NULL;
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        mqtt_inflight_item_t *it = &q->items[i];
        if (!it->in_use) continue;
        /* 无符号时间戳回绕安全：用差值比较 */
        if ((uint32_t)(now_ms - it->sent_at_ms) < timeout_ms) continue;

        /* ★ 重传必须置 DUP 位（固定头 bit3），否则对端无法识别这是重传 */
        it->pkt[0] |= 0x08;

        if (out_id)  *out_id  = it->pkt_id;
        if (out_pkt) *out_pkt = it->pkt;
        if (out_len) *out_len = it->len;
        return MQTT_OK;
    }
    return MQTT_ERR_UNSUPPORTED;    /* 没有需要重传的 */
}

int mqtt_inflight_mark_sent(mqtt_inflight_t *q, uint16_t id, uint32_t now_ms)
{
    if (!q || id == 0) return MQTT_ERR_NULL;
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        mqtt_inflight_item_t *it = &q->items[i];
        if (!it->in_use || it->pkt_id != id) continue;

        it->sent_at_ms = now_ms;
        it->retry++;
        if (it->retry >= MQTT_INFLIGHT_MAX_RETRY) {
            it->in_use = false;                     /* 重传次数用尽：丢弃 */
            it->len    = 0;
            return MQTT_ERR_MALFORMED;              /* 告知调用方已放弃 */
        }
        return MQTT_OK;
    }
    return MQTT_ERR_MALFORMED;
}

size_t mqtt_inflight_count(const mqtt_inflight_t *q)
{
    if (!q) return 0;
    size_t n = 0;
    for (size_t i = 0; i < MQTT_INFLIGHT_MAX; i++) {
        if (q->items[i].in_use) n++;
    }
    return n;
}
