#include "core/remote/mqtt_codec.h"
#include <string.h>

const char * mqtt_err_name(mqtt_err_t e)
{
    switch (e) {
        case MQTT_OK:              return "ok";
        case MQTT_ERR_NULL:        return "null";
        case MQTT_ERR_NO_SPACE:    return "no_space";
        case MQTT_ERR_MALFORMED:   return "malformed";
        case MQTT_ERR_TRUNCATED:   return "truncated";
        case MQTT_ERR_UNSUPPORTED: return "unsupported";
        default:                   return "unknown";
    }
}

/* ---------------- 变长剩余长度 ---------------- */

int mqtt_remaining_encode(uint32_t len, uint8_t *out, size_t cap, size_t *out_n)
{
    if (!out || !out_n) return MQTT_ERR_NULL;
    if (len > MQTT_REMAINING_MAX) return MQTT_ERR_MALFORMED;
    if (cap < 1) return MQTT_ERR_NO_SPACE;

    size_t n = 0;
    do {
        if (n >= cap || n >= 4) return MQTT_ERR_NO_SPACE;
        uint8_t byte = (uint8_t)(len & 0x7F);
        len >>= 7;
        if (len > 0) byte |= 0x80;      /* 还有后续字节 -> 置续位 */
        out[n++] = byte;
    } while (len > 0);

    *out_n = n;
    return MQTT_OK;
}

int mqtt_remaining_decode(const uint8_t *in, size_t in_n, uint32_t *out_len, size_t *out_n)
{
    if (!in || !out_len || !out_n) return MQTT_ERR_NULL;
    if (in_n < 1) return MQTT_ERR_TRUNCATED;

    uint32_t val = 0;
    uint32_t mul = 1;
    size_t   n   = 0;
    do {
        if (n >= 4) return MQTT_ERR_MALFORMED;      /* 第 4 字节仍带续位 = 非法 */
        if (n >= in_n) return MQTT_ERR_TRUNCATED;   /* 变长整数本身没读完 */
        uint8_t byte = in[n++];
        val += (uint32_t)(byte & 0x7F) * mul;
        if (val > MQTT_REMAINING_MAX) return MQTT_ERR_MALFORMED;
        if (!(byte & 0x80)) { *out_len = val; *out_n = n; return MQTT_OK; }
        mul *= 128;
    } while (1);
}

/* ---------------- 内部小工具 ---------------- */

/* UTF-8 字符串：2 字节长度前缀（大端）+ 内容（MQTT 3.1.1 §1.5.3） */
static int put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF); return 2; }

static int put_str(uint8_t *buf, size_t cap, size_t o, const char *s, size_t *out_n)
{
    size_t l = s ? strlen(s) : 0;
    if (l > 0xFFFF) return MQTT_ERR_MALFORMED;
    if (o + 2 + l > cap) return MQTT_ERR_NO_SPACE;
    buf[o++] = (uint8_t)(l >> 8);
    buf[o++] = (uint8_t)(l & 0xFF);
    if (l) memcpy(buf + o, s, l);
    *out_n = 2 + l;
    return MQTT_OK;
}

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* 取一个 UTF-8 字串；越界或长度不符一律报错（绝不跨出 view 的边界） */
static int get_str(const uint8_t *p, size_t len, size_t *off, const char **out, size_t *out_len)
{
    if (*off + 2 > len) return MQTT_ERR_TRUNCATED;
    size_t l = (size_t)get_u16(p + *off);
    *off += 2;
    if (*off + l > len) return MQTT_ERR_TRUNCATED;
    if (out)     *out     = (const char *)(p + *off);
    if (out_len) *out_len = l;
    *off += l;
    return MQTT_OK;
}

/* 固定头 = 类型<<4 | flags；后面跟变长剩余长度。返回已写入长度。 */
static int begin_fixed(uint8_t *buf, size_t cap, uint8_t type, uint8_t flags, uint32_t remaining, size_t *out_n)
{
    if (!buf || !out_n) return MQTT_ERR_NULL;
    if (cap < 1) return MQTT_ERR_NO_SPACE;
    buf[0] = (uint8_t)((type << 4) | (flags & 0x0F));
    size_t rn = 0;
    int e = mqtt_remaining_encode(remaining, buf + 1, cap - 1, &rn);
    if (e != MQTT_OK) return e;
    *out_n = 1 + rn;
    return MQTT_OK;
}

/* ---------------- 组包 ---------------- */

int mqtt_pack_connect(uint8_t *buf, size_t cap, size_t *out_n,
                      const char *client_id, uint16_t keepalive_s, bool clean_session,
                      const char *will_topic, const char *will_msg,
                      const char *username, const char *password)
{
    if (!buf || !out_n || !client_id) return MQTT_ERR_NULL;

    uint8_t  body[512];
    size_t   bo = 0;
    size_t   w  = 0;
    int      e;

    /* 可变头：协议名 "MQTT" + 级别 4 + 连接标志 + keepalive */
    if (bo + 6 > sizeof(body)) return MQTT_ERR_NO_SPACE;
    body[bo++] = 0x00; body[bo++] = 0x04;
    body[bo++] = 'M'; body[bo++] = 'Q'; body[bo++] = 'T'; body[bo++] = 'T';
    body[bo++] = 0x04;                                  /* protocol level */
    uint8_t cflag = clean_session ? 0x02 : 0x00;
    bool has_will = (will_topic && will_msg);
    if (has_will)  cflag |= 0x04;                       /* Will Flag */
    /* ★ R9 T04：Will QoS = 1（bit3）+ Will Retain = 1（bit5）。
     * 这两位是**固定**的，不是遗漏：LWT 的唯一用途是「设备异常掉线时让 broker
     * 代发一条离线通告」，QoS0 会在弱网下丢、非 retain 会让后上线的订阅者
     * 看不到这台设备是离线的 —— 两种都让 LWT 失去意义。本客户端不需要别的
     * 组合，故不把它做成参数（做成参数就得在调用方再写一遍同样的常数）。 */
    if (has_will)  cflag |= 0x08 | 0x20;
    if (username)  cflag |= 0x80;                       /* User Name Flag */
    if (password)  cflag |= 0x40;                       /* Password Flag */
    body[bo++] = cflag;
    bo += (size_t)put_u16(body + bo, keepalive_s);

    /* 载荷：client_id 必填，其余按标志 */
    e = put_str(body, sizeof(body), bo, client_id, &w); if (e != MQTT_OK) return e; bo += w;
    if (has_will) {
        e = put_str(body, sizeof(body), bo, will_topic, &w); if (e != MQTT_OK) return e; bo += w;
        e = put_str(body, sizeof(body), bo, will_msg,   &w); if (e != MQTT_OK) return e; bo += w;
    }
    if (username) { e = put_str(body, sizeof(body), bo, username, &w); if (e != MQTT_OK) return e; bo += w; }
    if (password) { e = put_str(body, sizeof(body), bo, password, &w); if (e != MQTT_OK) return e; bo += w; }

    size_t fn = 0;
    e = begin_fixed(buf, cap, MQTT_PKT_CONNECT, 0, (uint32_t)bo, &fn);
    if (e != MQTT_OK) return e;
    if (fn + bo > cap) return MQTT_ERR_NO_SPACE;
    memcpy(buf + fn, body, bo);
    *out_n = fn + bo;
    return MQTT_OK;
}

int mqtt_pack_publish(uint8_t *buf, size_t cap, size_t *out_n,
                      const char *topic, const uint8_t *payload, size_t payload_len,
                      uint16_t pkt_id, bool dup, uint8_t qos, bool retain)
{
    if (!buf || !out_n || !topic) return MQTT_ERR_NULL;
    if (qos > 1) return MQTT_ERR_UNSUPPORTED;     /* 本项目只需 QoS0 / QoS1 */

    uint8_t flags = (uint8_t)((qos & 0x03) << 1);
    if (dup)    flags |= 0x08;
    if (retain) flags |= 0x01;

    /* 先算可变头+载荷长度：topic(2+n) [+ pkt_id(2)] + payload */
    size_t tl = strlen(topic);
    if (tl > 0xFFFF) return MQTT_ERR_MALFORMED;
    size_t body_len = 2 + tl + ((qos > 0) ? 2u : 0u) + payload_len;

    size_t fn = 0;
    int e = begin_fixed(buf, cap, MQTT_PKT_PUBLISH, flags, (uint32_t)body_len, &fn);
    if (e != MQTT_OK) return e;
    if (fn + body_len > cap) return MQTT_ERR_NO_SPACE;

    size_t o = fn;
    buf[o++] = (uint8_t)(tl >> 8);
    buf[o++] = (uint8_t)(tl & 0xFF);
    memcpy(buf + o, topic, tl); o += tl;
    if (qos > 0) o += (size_t)put_u16(buf + o, pkt_id);
    if (payload_len) memcpy(buf + o, payload, payload_len);
    o += payload_len;

    *out_n = o;
    return MQTT_OK;
}

int mqtt_pack_puback(uint8_t *buf, size_t cap, size_t *out_n, uint16_t pkt_id)
{
    if (!buf || !out_n) return MQTT_ERR_NULL;
    size_t fn = 0;
    int e = begin_fixed(buf, cap, MQTT_PKT_PUBACK, 0, 2, &fn);
    if (e != MQTT_OK) return e;
    if (fn + 2 > cap) return MQTT_ERR_NO_SPACE;
    put_u16(buf + fn, pkt_id);
    *out_n = fn + 2;
    return MQTT_OK;
}

int mqtt_pack_subscribe(uint8_t *buf, size_t cap, size_t *out_n,
                        const char *topic, uint8_t qos, uint16_t pkt_id)
{
    if (!buf || !out_n || !topic) return MQTT_ERR_NULL;
    size_t tl = strlen(topic);
    if (tl > 0xFFFF) return MQTT_ERR_MALFORMED;

    size_t body_len = 2 /*pkt id*/ + 2 + tl + 1 /*qos*/;
    size_t fn = 0;
    int e = begin_fixed(buf, cap, MQTT_PKT_SUBSCRIBE, 0x02, (uint32_t)body_len, &fn);
    if (e != MQTT_OK) return e;
    if (fn + body_len > cap) return MQTT_ERR_NO_SPACE;

    size_t o = fn;
    o += (size_t)put_u16(buf + o, pkt_id);
    buf[o++] = (uint8_t)(tl >> 8);
    buf[o++] = (uint8_t)(tl & 0xFF);
    memcpy(buf + o, topic, tl); o += tl;
    buf[o++] = qos;
    *out_n = o;
    return MQTT_OK;
}

int mqtt_pack_pingreq(uint8_t *buf, size_t cap, size_t *out_n)
{
    if (!buf || !out_n) return MQTT_ERR_NULL;
    size_t fn = 0;
    int e = begin_fixed(buf, cap, MQTT_PKT_PINGREQ, 0, 0, &fn);
    if (e != MQTT_OK) return e;
    *out_n = fn;
    return MQTT_OK;
}

int mqtt_pack_disconnect(uint8_t *buf, size_t cap, size_t *out_n)
{
    if (!buf || !out_n) return MQTT_ERR_NULL;
    size_t fn = 0;
    int e = begin_fixed(buf, cap, MQTT_PKT_DISCONNECT, 0, 0, &fn);
    if (e != MQTT_OK) return e;
    *out_n = fn;
    return MQTT_OK;
}

/* ---------------- 解析 ---------------- */

int mqtt_parse(const uint8_t *buf, size_t len, mqtt_pkt_view_t *out, size_t *consumed)
{
    if (!buf || !out || !consumed) return MQTT_ERR_NULL;
    *consumed = 0;
    if (len < 2) return MQTT_ERR_TRUNCATED;      /* 至少要有类型+1字节长度 */

    uint8_t b0     = buf[0];
    uint8_t type   = (uint8_t)(b0 >> 4);
    uint8_t flags  = (uint8_t)(b0 & 0x0F);

    uint32_t rem = 0;
    size_t   rn  = 0;
    int e = mqtt_remaining_decode(buf + 1, len - 1, &rem, &rn);
    if (e != MQTT_OK) return e;                  /* TRUNCATED 或 MALFORMED 原样透传 */

    size_t total = 1 + rn + (size_t)rem;
    if (len < total) return MQTT_ERR_TRUNCATED;  /* 半包：剩余长度声明的比实际收到的多 */

    out->type        = (mqtt_pkt_type_t)type;
    out->flags       = flags;
    out->remaining   = rem;
    out->payload     = buf + 1 + rn;
    out->payload_len = (size_t)rem;
    out->total_len   = total;
    *consumed        = total;
    return MQTT_OK;
}

int mqtt_parse_connack(const mqtt_pkt_view_t *v, bool *session_present, uint8_t *ret_code)
{
    if (!v) return MQTT_ERR_NULL;
    if (v->type != MQTT_PKT_CONNACK) return MQTT_ERR_MALFORMED;
    if (v->payload_len < 2) return MQTT_ERR_TRUNCATED;
    if (session_present) *session_present = (v->payload[0] & 0x01) != 0;
    if (ret_code)        *ret_code        = v->payload[1];
    return MQTT_OK;
}

int mqtt_parse_publish(const mqtt_pkt_view_t *v, char *topic, size_t topic_cap,
                       const uint8_t **payload, size_t *payload_len,
                       uint16_t *pkt_id, bool *dup, uint8_t *qos, bool *retain)
{
    if (!v) return MQTT_ERR_NULL;
    if (v->type != MQTT_PKT_PUBLISH) return MQTT_ERR_MALFORMED;

    if (dup)    *dup    = (v->flags & 0x08) != 0;
    if (retain) *retain = (v->flags & 0x01) != 0;
    uint8_t q = (uint8_t)((v->flags >> 1) & 0x03);
    if (qos)    *qos    = q;

    size_t off = 0;
    const char *ts = NULL;
    size_t tl = 0;
    int e = get_str(v->payload, v->payload_len, &off, &ts, &tl);
    if (e != MQTT_OK) return e;

    if (topic && topic_cap) {
        /* 拷进调用方缓冲并补 '\0'；空间不足则截断（仍保证有结尾符） */
        size_t n = (tl < topic_cap - 1) ? tl : (topic_cap - 1);
        memcpy(topic, ts, n);
        topic[n] = '\0';
    }

    if (q > 0) {
        if (off + 2 > v->payload_len) return MQTT_ERR_TRUNCATED;
        if (pkt_id) *pkt_id = get_u16(v->payload + off);
        off += 2;
    } else if (pkt_id) {
        *pkt_id = 0;                     /* QoS0 无 packet identifier */
    }

    if (payload)     *payload     = v->payload + off;
    if (payload_len) *payload_len = v->payload_len - off;
    return MQTT_OK;
}

int mqtt_parse_puback(const mqtt_pkt_view_t *v, uint16_t *pkt_id)
{
    if (!v) return MQTT_ERR_NULL;
    if (v->type != MQTT_PKT_PUBACK) return MQTT_ERR_MALFORMED;
    if (v->payload_len < 2) return MQTT_ERR_TRUNCATED;
    if (pkt_id) *pkt_id = get_u16(v->payload);
    return MQTT_OK;
}

int mqtt_parse_suback(const mqtt_pkt_view_t *v, uint16_t *pkt_id, uint8_t *ret_code)
{
    if (!v) return MQTT_ERR_NULL;
    if (v->type != MQTT_PKT_SUBACK) return MQTT_ERR_MALFORMED;
    if (v->payload_len < 3) return MQTT_ERR_TRUNCATED;
    if (pkt_id)   *pkt_id   = get_u16(v->payload);
    if (ret_code) *ret_code = v->payload[2];
    return MQTT_OK;
}
