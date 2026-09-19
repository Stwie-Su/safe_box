/**
 * @file test_mqtt_inflight.c
 * R9 T02：未确认报文队列单测。
 *
 * 重点验证：**队列存的是字节而非指针**（入队后改坏源缓冲，队列内容必须不变），
 * 以及 id 回绕、DUP 置位、重传次数用尽后丢弃、队列满与各类非法输入。
 */
#include "test_util.h"

#include <string.h>

#include "core/remote/mqtt_inflight.h"
#include "core/remote/mqtt_codec.h"

/* 造一条 QoS1 的 PUBLISH（真实走 codec，顺便当集成测试） */
static size_t make_publish(uint8_t *buf, size_t cap, uint16_t id, const char *msg)
{
    size_t n = 0;
    CHECK(mqtt_pack_publish(buf, cap, &n, "safe/log", (const uint8_t *)msg,
                            strlen(msg), id, false, 1, false) == MQTT_OK);
    return n;
}

/* ---------- 1. packet id 分配与回绕 ---------- */
static void t_id_alloc(void)
{
    mqtt_inflight_t q;
    mqtt_inflight_init(&q);

    CHECK(mqtt_inflight_next_id(&q) == 1);
    CHECK(mqtt_inflight_next_id(&q) == 2);
    CHECK(mqtt_inflight_next_id(&q) == 3);

    /* 回绕：到 65535 之后必须回到 1，且**永不返回 0** */
    q.next_id = 0xFFFE;
    CHECK(mqtt_inflight_next_id(&q) == 0xFFFF);
    CHECK(mqtt_inflight_next_id(&q) == 1);

    /* 即便内部被写坏成 0xFFFF 也能正确回绕 */
    q.next_id = 0xFFFF;
    CHECK(mqtt_inflight_next_id(&q) == 1);

    /* id=0 是非法值，不得入队 */
    uint8_t pkt[64];
    size_t  n = make_publish(pkt, sizeof(pkt), 7, "x");
    CHECK(mqtt_inflight_add(&q, 0, pkt, n, 100) == MQTT_ERR_MALFORMED);
}

/* ---------- 2. 入队 / 查找 / ACK ---------- */
static void t_add_find_ack(void)
{
    mqtt_inflight_t q;
    mqtt_inflight_init(&q);

    uint8_t pkt[64];
    size_t  n = make_publish(pkt, sizeof(pkt), 1001, "hello");

    CHECK(mqtt_inflight_count(&q) == 0);
    CHECK(mqtt_inflight_add(&q, 1001, pkt, n, 1000) == MQTT_OK);
    CHECK(mqtt_inflight_count(&q) == 1);

    const mqtt_inflight_item_t *it = mqtt_inflight_find(&q, 1001);
    CHECK(it != NULL);
    CHECK(it->len == n);
    CHECK(it->retry == 0);
    CHECK(it->sent_at_ms == 1000);

    /* 同一 id 不得重复入队（否则 ACK 时分不清是哪一条） */
    CHECK(mqtt_inflight_add(&q, 1001, pkt, n, 1000) == MQTT_ERR_MALFORMED);

    /* 未找到 */
    CHECK(mqtt_inflight_find(&q, 9999) == NULL);

    /* ACK 后应出队 */
    CHECK(mqtt_inflight_ack(&q, 1001) == MQTT_OK);
    CHECK(mqtt_inflight_count(&q) == 0);
    CHECK(mqtt_inflight_find(&q, 1001) == NULL);

    /* 重复 / 迟到的 PUBACK：忽略，不是错误 */
    CHECK(mqtt_inflight_ack(&q, 1001) == MQTT_ERR_MALFORMED);
}

/* ---------- 3. ★ 存字节而非指针 ---------- */
static void t_stores_bytes_not_pointer(void)
{
    mqtt_inflight_t q;
    mqtt_inflight_init(&q);

    uint8_t pkt[64];
    size_t  n = make_publish(pkt, sizeof(pkt), 2002, "keepme");
    CHECK(mqtt_inflight_add(&q, 2002, pkt, n, 500) == MQTT_OK);

    /* 入队之后，把**源缓冲彻底改坏**（模拟 rpc.c 释放/复用 payload） */
    memset(pkt, 0x5A, sizeof(pkt));
    memcpy(pkt, "GARBAGE-GARBAGE", 15);

    const mqtt_inflight_item_t *it = mqtt_inflight_find(&q, 2002);
    CHECK(it != NULL);
    /* 队列里的字节必须还是原始那条，且能正确解析出原 topic 与内容 */
    mqtt_pkt_view_t v;
    size_t consumed = 0;
    CHECK(mqtt_parse(it->pkt, it->len, &v, &consumed) == MQTT_OK);
    CHECK(v.type == MQTT_PKT_PUBLISH);

    char tbuf[32];
    const uint8_t *pl = NULL;
    size_t pll = 0;
    uint16_t pid = 0;
    CHECK(mqtt_parse_publish(&v, tbuf, sizeof(tbuf), &pl, &pll, &pid, NULL, NULL, NULL) == MQTT_OK);
    CHECK(strcmp(tbuf, "safe/log") == 0);
    CHECK(pll == 6);
    CHECK(memcmp(pl, "keepme", 6) == 0);
    CHECK(pid == 2002);
}

/* ---------- 4. 超时扫描 + DUP 置位 + 重传次数 ---------- */
static void t_timeout_and_retry(void)
{
    mqtt_inflight_t q;
    mqtt_inflight_init(&q);

    uint8_t pkt[64];
    size_t  n = make_publish(pkt, sizeof(pkt), 3003, "retry-me");
    CHECK(mqtt_inflight_add(&q, 3003, pkt, n, 1000) == MQTT_OK);

    /* 还没超时 → 不应扫出 */
    const uint8_t *out_pkt = NULL;
    size_t out_len = 0;
    uint16_t out_id = 0;
    CHECK(mqtt_inflight_timeout_scan(&q, 1500, 1000, &out_id, &out_pkt, &out_len)
          == MQTT_ERR_UNSUPPORTED);

    /* 已超时 → 扫出，且 **DUP 位被置 1** */
    CHECK(mqtt_inflight_timeout_scan(&q, 2000, 1000, &out_id, &out_pkt, &out_len) == MQTT_OK);
    CHECK(out_id  == 3003);
    CHECK(out_len == n);
    CHECK((out_pkt[0] & 0x08) != 0);        /* DUP = bit3 */

    /* 发出去后刷新：sent_at 更新、retry 累加 */
    CHECK(mqtt_inflight_mark_sent(&q, 3003, 2000) == MQTT_OK);
    const mqtt_inflight_item_t *it = mqtt_inflight_find(&q, 3003);
    CHECK(it != NULL);
    CHECK(it->retry == 1);
    CHECK(it->sent_at_ms == 2000);

    /* 再超时、再重传，直到次数用尽被丢弃 */
    CHECK(mqtt_inflight_timeout_scan(&q, 3200, 1000, &out_id, &out_pkt, &out_len) == MQTT_OK);
    CHECK(mqtt_inflight_mark_sent(&q, 3003, 3200) == MQTT_OK);   /* retry=2 */

    CHECK(mqtt_inflight_timeout_scan(&q, 4400, 1000, &out_id, &out_pkt, &out_len) == MQTT_OK);
    /* 第三次：retry 达到 MQTT_INFLIGHT_MAX_RETRY → 丢弃并明确告知 */
    CHECK(mqtt_inflight_mark_sent(&q, 3003, 4400) == MQTT_ERR_MALFORMED);
    CHECK(mqtt_inflight_count(&q) == 0);
}

/* ---------- 5. 队列满与非法输入 ---------- */
static void t_limits(void)
{
    mqtt_inflight_t q;
    mqtt_inflight_init(&q);

    uint8_t pkt[64];
    size_t  n = make_publish(pkt, sizeof(pkt), 1, "m");

    /* 填满 */
    for (uint16_t i = 1; i <= MQTT_INFLIGHT_MAX; i++) {
        CHECK(mqtt_inflight_add(&q, i, pkt, n, 10) == MQTT_OK);
    }
    CHECK(mqtt_inflight_count(&q) == MQTT_INFLIGHT_MAX);

    /* 再入队 → 明确 NO_SPACE（静默丢弃会让上层以为发出去了） */
    CHECK(mqtt_inflight_add(&q, 99, pkt, n, 10) == MQTT_ERR_NO_SPACE);

    /* 腾一个位置后又能入队 */
    CHECK(mqtt_inflight_ack(&q, 3) == MQTT_OK);
    CHECK(mqtt_inflight_add(&q, 99, pkt, n, 10) == MQTT_OK);

    /* 报文过长 → 拒绝（不截断） */
    uint8_t big[MQTT_INFLIGHT_PKT_MAX + 8];
    memset(big, 0x41, sizeof(big));
    CHECK(mqtt_inflight_add(&q, 200, big, sizeof(big), 10) == MQTT_ERR_NO_SPACE);

    /* 空指针与零长度 */
    CHECK(mqtt_inflight_add(&q, 201, NULL, n, 10) == MQTT_ERR_NULL);
    CHECK(mqtt_inflight_add(&q, 201, pkt, 0,  10) == MQTT_ERR_NO_SPACE);

    /* 空队列的安全调用 */
    mqtt_inflight_t e;
    mqtt_inflight_init(&e);
    uint16_t dummy_id = 0; const uint8_t *dummy_p = NULL; size_t dummy_l = 0;
    CHECK(mqtt_inflight_timeout_scan(&e, 9999, 1000, &dummy_id, &dummy_p, &dummy_l)
          == MQTT_ERR_UNSUPPORTED);
    CHECK(mqtt_inflight_count(&e) == 0);
    CHECK(mqtt_inflight_count(NULL) == 0);
    CHECK(mqtt_inflight_find(&e, 0) == NULL);
}

int main(void)
{
    t_id_alloc();
    t_add_find_ack();
    t_stores_bytes_not_pointer();
    t_timeout_and_retry();
    t_limits();
    TEST_RESULT();
}
