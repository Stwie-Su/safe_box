/**
 * @file test_mqtt_codec.c
 * R9 自研 MQTT 客户端 T01：协议编解码层单测。
 *
 * 覆盖：变长剩余长度边界与畸形、组包→解析 round-trip、
 *       **半包（拆两次喂入）与粘包（两个包一次喂入）**、缓冲不足与非法输入。
 * 本层是纯函数（不 IO / 不取时间 / 不 malloc），故测试完全确定性。
 */
#include "test_util.h"

#include <string.h>
#include <stdio.h>

#include "core/remote/mqtt_codec.h"

/* ---------- 1. 变长剩余长度 ---------- */
static void t_remaining(void)
{
    uint8_t  b[8];
    size_t   n = 0;
    uint32_t v = 0;
    size_t   rn = 0;

    /* 边界：1 / 2 / 3 / 4 字节的临界值 */
    const uint32_t cases[] = { 0u, 127u, 128u, 16383u, 16384u,
                               2097151u, 2097152u, 268435455u };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        CHECK(mqtt_remaining_encode(cases[i], b, sizeof(b), &n) == MQTT_OK);
        CHECK(mqtt_remaining_decode(b, n, &v, &rn) == MQTT_OK);
        CHECK(v  == cases[i]);
        CHECK(rn == n);
    }

    /* 越界：超过 268435455 应报非法 */
    CHECK(mqtt_remaining_encode(268435456u, b, sizeof(b), &n) == MQTT_ERR_MALFORMED);

    /* 畸形：4 个字节**都**带续位（协议规定最多 4 字节）→ 必须报错，不得无限延长 */
    const uint8_t bad[] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(mqtt_remaining_decode(bad, sizeof(bad), &v, &rn) == MQTT_ERR_MALFORMED);

    /* 半包：变长整数本身没读完 */
    const uint8_t half[] = { 0x80 };            /* 声明还有后续字节，但没有 */
    CHECK(mqtt_remaining_decode(half, sizeof(half), &v, &rn) == MQTT_ERR_TRUNCATED);

    /* 缓冲不足 */
    CHECK(mqtt_remaining_encode(128u, b, 1, &n) == MQTT_ERR_NO_SPACE);
}

/* ---------- 2. CONNECT round-trip ---------- */
static void t_connect(void)
{
    uint8_t buf[256];
    size_t  n = 0;
    CHECK(mqtt_pack_connect(buf, sizeof(buf), &n, "safe-001", 60, true,
                            "safe/status", "offline", "dev", "secret") == MQTT_OK);
    CHECK(n > 0);
    /* 首字节 = 0x10 (CONNECT<<4) */
    CHECK(buf[0] == 0x10);

    mqtt_pkt_view_t v;
    size_t consumed = 0;
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    CHECK(v.type        == MQTT_PKT_CONNECT);
    CHECK(consumed      == n);
    CHECK(v.payload_len >= 10);
    /* 可变头里应能看到协议名 MQTT 与级别 4 */
    CHECK(memcmp(v.payload + 2, "MQTT", 4) == 0);
    CHECK(v.payload[6]  == 0x04);
    /* 带 will + user + pass + clean session → 标志位 0xC6 */
    CHECK(v.payload[7]  == (uint8_t)0xC6);
}

/* ---------- 3. PUBLISH round-trip（QoS0 / QoS1） ---------- */
static void t_publish(void)
{
    uint8_t buf[256];
    size_t  n = 0;
    const char * topic = "safe/log";
    const char * msg   = "hello";

    /* QoS0：不带 packet id */
    CHECK(mqtt_pack_publish(buf, sizeof(buf), &n, topic, (const uint8_t *)msg,
                            strlen(msg), 0, false, 0, false) == MQTT_OK);
    mqtt_pkt_view_t v;
    size_t consumed = 0;
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    CHECK(v.type == MQTT_PKT_PUBLISH);

    char        tbuf[32];
    const uint8_t * pl = NULL;
    size_t      pll = 0;
    uint16_t    pid = 0;
    bool        dup = true, ret = true;
    uint8_t     qos = 9;
    CHECK(mqtt_parse_publish(&v, tbuf, sizeof(tbuf), &pl, &pll, &pid, &dup, &qos, &ret) == MQTT_OK);
    CHECK(strcmp(tbuf, topic) == 0);
    CHECK(pll == strlen(msg));
    CHECK(memcmp(pl, msg, pll) == 0);
    CHECK(pid == 0);          /* QoS0 无 packet identifier */
    CHECK(qos == 0);
    CHECK(dup == false);
    CHECK(ret == false);

    /* QoS1：带 packet id，且可带 DUP 标志 */
    CHECK(mqtt_pack_publish(buf, sizeof(buf), &n, topic, (const uint8_t *)msg,
                            strlen(msg), 4242, true, 1, true) == MQTT_OK);
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    CHECK(mqtt_parse_publish(&v, tbuf, sizeof(tbuf), &pl, &pll, &pid, &dup, &qos, &ret) == MQTT_OK);
    CHECK(pid == 4242);
    CHECK(qos == 1);
    CHECK(dup == true);
    CHECK(ret == true);

    /* QoS2 本客户端不支持 → 明确拒绝，而不是悄悄组出错误报文 */
    CHECK(mqtt_pack_publish(buf, sizeof(buf), &n, topic, (const uint8_t *)msg,
                            strlen(msg), 1, false, 2, false) == MQTT_ERR_UNSUPPORTED);
}

/* ---------- 4. 短报文与回执 ---------- */
static void t_small_pkts(void)
{
    uint8_t buf[16];
    size_t  n = 0;
    mqtt_pkt_view_t v;
    size_t consumed = 0;

    CHECK(mqtt_pack_puback(buf, sizeof(buf), &n, 77) == MQTT_OK);
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    uint16_t pid = 0;
    CHECK(mqtt_parse_puback(&v, &pid) == MQTT_OK);
    CHECK(pid == 77);

    CHECK(mqtt_pack_pingreq(buf, sizeof(buf), &n) == MQTT_OK);
    CHECK(n == 2);                        /* 0xC0 0x00 */
    CHECK(buf[0] == 0xC0);
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    CHECK(v.type == MQTT_PKT_PINGREQ);

    CHECK(mqtt_pack_disconnect(buf, sizeof(buf), &n) == MQTT_OK);
    CHECK(n == 2);
    CHECK(buf[0] == 0xE0);
    CHECK(mqtt_parse(buf, n, &v, &consumed) == MQTT_OK);
    CHECK(v.type == MQTT_PKT_DISCONNECT);

    /* SUBSCRIBE → SUBACK（后者通常是 broker 回的，这里验证解析侧） */
    uint8_t sbuf[64];
    size_t  sn = 0;
    CHECK(mqtt_pack_subscribe(sbuf, sizeof(sbuf), &sn, "safe/cmd", 1, 9) == MQTT_OK);
    CHECK(mqtt_parse(sbuf, sn, &v, &consumed) == MQTT_OK);
    CHECK(v.type == MQTT_PKT_SUBSCRIBE);

    const uint8_t suback[] = { 0x90, 0x03, 0x00, 0x09, 0x01 };
    CHECK(mqtt_parse(suback, sizeof(suback), &v, &consumed) == MQTT_OK);
    uint8_t rc = 0;
    CHECK(mqtt_parse_suback(&v, &pid, &rc) == MQTT_OK);
    CHECK(pid == 9);
    CHECK(rc  == 1);          /* granted QoS1 */

    /* CONNACK */
    const uint8_t connack[] = { 0x20, 0x02, 0x01, 0x00 };
    CHECK(mqtt_parse(connack, sizeof(connack), &v, &consumed) == MQTT_OK);
    bool sp = false; uint8_t crc = 0xFF;
    CHECK(mqtt_parse_connack(&v, &sp, &crc) == MQTT_OK);
    CHECK(sp  == true);
    CHECK(crc == 0);
}

/* ---------- 5. 半包与粘包（协议解析的核心考点） ---------- */
static void t_frag_and_coalesce(void)
{
    uint8_t p1[64], p2[64];
    size_t  n1 = 0, n2 = 0;
    CHECK(mqtt_pack_publish(p1, sizeof(p1), &n1, "safe/a", (const uint8_t *)"AAA", 3, 0, false, 0, false) == MQTT_OK);
    CHECK(mqtt_pack_publish(p2, sizeof(p2), &n2, "safe/b", (const uint8_t *)"BB",  2, 0, false, 0, false) == MQTT_OK);

    /* ---- 半包：只给前一个包的前一半 → 必须报 TRUNCATED 且不消耗任何字节 ---- */
    mqtt_pkt_view_t v;
    size_t consumed = 0;
    size_t half = n1 / 2;
    CHECK(half >= 2);
    CHECK(mqtt_parse(p1, half, &v, &consumed) == MQTT_ERR_TRUNCATED);
    CHECK(consumed == 0);          /* 没解析出完整报文，游标不动 */

    /* 补足后半段 → 解析成功，且总长等于 n1 */
    uint8_t full[128];
    memcpy(full, p1, n1);
    CHECK(mqtt_parse(full, n1, &v, &consumed) == MQTT_OK);
    CHECK(consumed == n1);

    /* ---- 粘包：两个包拼在一次缓冲区里 → 应能连续解析出两个 ---- */
    uint8_t both[256];
    memcpy(both,      p1, n1);
    memcpy(both + n1, p2, n2);

    mqtt_pkt_view_t v1, v2;
    size_t c1 = 0, c2 = 0;
    CHECK(mqtt_parse(both,           n1 + n2, &v1, &c1) == MQTT_OK);
    CHECK(mqtt_parse(both + c1, n1 + n2 - c1, &v2, &c2) == MQTT_OK);
    CHECK(c1 == n1);
    CHECK(c2 == n2);
    CHECK(v1.type == MQTT_PKT_PUBLISH);
    CHECK(v2.type == MQTT_PKT_PUBLISH);

    char t1[16], t2[16];
    CHECK(mqtt_parse_publish(&v1, t1, sizeof(t1), NULL, NULL, NULL, NULL, NULL, NULL) == MQTT_OK);
    CHECK(mqtt_parse_publish(&v2, t2, sizeof(t2), NULL, NULL, NULL, NULL, NULL, NULL) == MQTT_OK);
    CHECK(strcmp(t1, "safe/a") == 0);
    CHECK(strcmp(t2, "safe/b") == 0);

    /* 粘包 + 尾部半包：第二个包被截断 → 第二个应报 TRUNCATED */
    CHECK(mqtt_parse(both, n1 + n2 - 1, &v1, &c1) == MQTT_OK);
    CHECK(mqtt_parse(both + c1, n1 + n2 - 1 - c1, &v2, &c2) == MQTT_ERR_TRUNCATED);
}

/* ---------- 6. 缓冲不足与非法输入 ---------- */
static void t_limits(void)
{
    uint8_t tiny[4];
    size_t  n = 0;
    /* 缓冲太小：不应越界写，只返回 NO_SPACE */
    CHECK(mqtt_pack_connect(tiny, sizeof(tiny), &n, "id", 60, true, NULL, NULL, NULL, NULL)
          == MQTT_ERR_NO_SPACE);

    /* 空指针一律 NULL，不得崩溃 */
    CHECK(mqtt_pack_publish(NULL, 8, &n, "t", NULL, 0, 0, false, 0, false) == MQTT_ERR_NULL);
    CHECK(mqtt_pack_publish(tiny, sizeof(tiny), NULL, "t", NULL, 0, 0, false, 0, false) == MQTT_ERR_NULL);
    CHECK(mqtt_pack_publish(tiny, sizeof(tiny), &n, NULL, NULL, 0, 0, false, 0, false) == MQTT_ERR_NULL);
    CHECK(mqtt_parse(NULL, 8, NULL, &n) == MQTT_ERR_NULL);

    /* 长度不足 2 字节（类型 + 至少 1 字节长度）→ 半包 */
    const uint8_t one[] = { 0x20 };
    mqtt_pkt_view_t v;
    size_t c = 0;
    CHECK(mqtt_parse(one, sizeof(one), &v, &c) == MQTT_ERR_TRUNCATED);

    /* 把 PUBACK 当成 CONNACK 解析 → 应报类型不符 */
    uint8_t buf[16];
    CHECK(mqtt_pack_puback(buf, sizeof(buf), &n, 1) == MQTT_OK);
    CHECK(mqtt_parse(buf, n, &v, &c) == MQTT_OK);
    bool sp = false; uint8_t rc = 0;
    CHECK(mqtt_parse_connack(&v, &sp, &rc) == MQTT_ERR_MALFORMED);
}

int main(void)
{
    t_remaining();
    t_connect();
    t_publish();
    t_small_pkts();
    t_frag_and_coalesce();
    t_limits();
    TEST_RESULT();
}
