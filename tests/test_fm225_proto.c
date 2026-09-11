/**
 * @file test_fm225_proto.c
 * FM225 协议解析器单元测试（Sprint3 步骤 3c，规约 §5.20）。
 *
 * 覆盖四类边界（c2 验收）：
 *   1) 半包：一次一个字节喂（最细粒度），跨调用累积成帧；
 *   2) 粘包：两帧连在一起一次喂，回调连续触发且内容正确；
 *   3) 坏校验（XOR 错）：整帧丢弃 + 后续好帧仍可解（重同步）；
 *   4) 超时：半帧跨 tick 推进超时作废，统计计数 + 状态复位；
 *  另含：手册示例帧验算（EF AA 00 00 05 13 00 00 03 1F 0A）、NOTE READY 帧、
 *  垃圾前导字节（帧前有噪声）、0xEF 出现在 Data 区不误触发帧头、超长帧丢弃。
 *
 * 纯状态机验证：不碰串口、不碰线程——fm225_proto_feed 直驱。
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "hal/face/fm225_proto.h"

/* ---------------- 帧构造与回调收集 ---------------- */

#define MAX_RECV 8

typedef struct {
    fm225_frame_t frames[MAX_RECV];
    int n;
} recv_t;

static void on_frame(const fm225_frame_t * f, void * user)
{
    recv_t * r = (recv_t *)user;
    if(r->n < MAX_RECV) r->frames[r->n++] = *f;   /* 拷走：data 指针回调返回即失效 */
}

/* 构造一帧：EF AA + msgid + size(大端) + data + xor。
 * xor_out 为 NULL 时自动算正确 XOR；非 NULL 时写入指定值（构造坏帧用）。 */
static size_t build_frame(uint8_t * out, uint8_t msgid,
                           const uint8_t * data, uint16_t len, const uint8_t * xor_out)
{
    size_t k = 0;
    out[k++] = FM225_SYNC0;
    out[k++] = FM225_SYNC1;
    out[k++] = msgid;
    out[k++] = (uint8_t)(len >> 8);
    out[k++] = (uint8_t)(len & 0xFF);
    uint8_t x = msgid ^ (uint8_t)(len >> 8) ^ (uint8_t)(len & 0xFF);
    for(uint16_t i = 0; i < len; i++) { out[k++] = data[i]; x ^= data[i]; }
    out[k++] = xor_out ? *xor_out : x;
    return k;
}

/* ---------------- 用例 1：手册示例帧（验算基准） ---------------- */

static void test_manual_example_frame(void)
{
    /* 手册 §M>>H REPLY 协议示例：EF AA 00 00 05 13 00 00 03 1F 0A
     * REPLY(0x00)，mid=ENROLL(0x13)，result=MR_SUCCESS(0)，user_id_heb=0，
     * user_id_leb=3，face_direction=0x1F；XOR=0x0A（手册明示，已人工验算通过） */
    static const uint8_t raw[] = {
        0xEF, 0xAA, 0x00, 0x00, 0x05, 0x13, 0x00, 0x00, 0x03, 0x1F, 0x0A
    };

    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    fm225_proto_feed(&ctx, raw, sizeof(raw));

    CHECK(r.n == 1);
    CHECK(r.frames[0].msgid == FM225_MSGID_REPLY);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_ENROLL);
    CHECK(r.frames[0].result == FM225_MR_SUCCESS);
    CHECK(r.frames[0].data_len == 3);
    CHECK(r.frames[0].data[0] == 0x00);   /* user_id_heb */
    CHECK(r.frames[0].data[1] == 0x03);   /* user_id_leb → user_id=3 */
    CHECK(r.frames[0].data[2] == 0x1F);   /* face_direction */

    size_t frames = fm225_proto_stats(&ctx, NULL, NULL, NULL);
    CHECK(frames == 1);
}

/* ---------------- 用例 2：半包（逐字节喂） ---------------- */

static void test_half_packet(void)
{
    /* REPLY(VERIFY) 应答：mid=0x12, result=MR_SUCCESS, user_id=3（heb=0, leb=3）
     * —— verify 成功、用户 ID=3 的最小应答帧 */
    uint8_t raw[64];
    const uint8_t d[] = { 0x12, 0x00, 0x00, 0x03 };
    size_t n = build_frame(raw, FM225_MSGID_REPLY, d, 4, NULL);
    CHECK(n == 2 + 1 + 2 + 4 + 1);   /* sync + msgid + size + data + xor = 10 */

    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    /* 一个字节一个字节喂：中途任何时刻都只可能没解完，不可能出错帧 */
    for(size_t i = 0; i < n; i++) {
        fm225_proto_feed(&ctx, &raw[i], 1);
        /* 半途不应出帧（最后一字节 XOR 落地才成帧） */
        CHECK(r.n == ((i == n - 1) ? 1 : 0));
    }

    CHECK(r.n == 1);
    CHECK(r.frames[0].msgid == FM225_MSGID_REPLY);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_VERIFY);
    CHECK(r.frames[0].result == FM225_MR_SUCCESS);
    CHECK(r.frames[0].data[1] == 0x03);
}

/* ---------------- 用例 3：粘包（多帧一次喂） ---------------- */

static void test_sticky_packets(void)
{
    /* 两帧拼一起 + 中间夹 0 字节间隔；再验证帧尾恰好是 0xEF 时下一帧开头不被吞 */
    uint8_t buf[128];
    size_t k = 0;

    const uint8_t d1[] = { 0x12, 0x00, 0x00, 0x07 };                       /* verify OK uid=7 */
    k += build_frame(buf + k, FM225_MSGID_REPLY, d1, 4, NULL);

    const uint8_t d2[] = { 0x00 };                                        /* NOTE READY */
    k += build_frame(buf + k, FM225_MSGID_NOTE, d2, 1, NULL);

    /* 第三帧：verify 活体失败（mid=0x12, result=MR_FAILED4_LIVENESSCHECK），
     * Data 含 0xEF 字节——验证 Data 区的 0xEF 不会被误当帧头 */
    const uint8_t d3[] = { 0x12, 0x0C, 0xEF, 0xAA, 0x00 };
    k += build_frame(buf + k, FM225_MSGID_REPLY, d3, 5, NULL);

    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    fm225_proto_feed(&ctx, buf, k);   /* 一次灌入三帧 */

    CHECK(r.n == 3);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_VERIFY);
    CHECK(r.frames[0].result == FM225_MR_SUCCESS);
    CHECK(r.frames[1].msgid == FM225_MSGID_NOTE);
    CHECK(r.frames[1].mid_or_nid == FM225_NID_READY);
    CHECK(r.frames[2].mid_or_nid == FM225_CMD_VERIFY);
    CHECK(r.frames[2].result == FM225_MR_FAILED4_LIVENESS);
    CHECK(r.frames[2].data_len == 3);
    CHECK(r.frames[2].data[0] == 0xEF);   /* Data 区 0xEF 原样到达 */
    CHECK(r.frames[2].data[1] == 0xAA);

    size_t frames = fm225_proto_stats(&ctx, NULL, NULL, NULL);
    CHECK(frames == 3);
}

/* ---------------- 用例 4：坏校验 → 丢弃并重同步 ---------------- */

static void test_bad_checksum(void)
{
    uint8_t buf[128];
    size_t k = 0;

    /* 帧 1：好帧（verify no-match，用 MR_FAILED4_UNKNOWNUSER 表示无匹配用户） */
    const uint8_t d1[] = { 0x12, 0x08 };
    k += build_frame(buf + k, FM225_MSGID_REPLY, d1, 2, NULL);

    /* 帧 2：坏 XOR（正确值 +1） */
    const uint8_t d2[] = { 0x13, 0x00, 0x00, 0x05 };
    uint8_t raw[64];
    size_t n2 = build_frame(raw, FM225_MSGID_REPLY, d2, 4, NULL);
    raw[n2 - 1] ^= 0x01;                  /* 破坏 XOR */
    memcpy(buf + k, raw, n2);
    k += n2;

    /* 帧 3：好帧（NOTE READY）——坏帧后必须仍可解（重同步成功） */
    const uint8_t d3[] = { 0x00 };
    k += build_frame(buf + k, FM225_MSGID_NOTE, d3, 1, NULL);

    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    fm225_proto_feed(&ctx, buf, k);

    /* 坏帧被丢弃：只解出帧 1 和帧 3 */
    CHECK(r.n == 2);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_VERIFY);
    CHECK(r.frames[0].result == FM225_MR_FAILED4_UNKNOWNUSER);
    CHECK(r.frames[1].msgid == FM225_MSGID_NOTE);
    CHECK(r.frames[1].mid_or_nid == FM225_NID_READY);

    size_t frames, bad = 0;
    frames = fm225_proto_stats(&ctx, &bad, NULL, NULL);
    CHECK(frames == 2);
    CHECK(bad == 1);
}

/* 坏校验 + 噪声尾巴 + 帧头埋在坏帧 Data 里：逐字节重同步必须能捞回后继帧 */
static void test_bad_checksum_resync_from_noise(void)
{
    uint8_t buf[128];
    size_t k = 0;

    /* 帧头噪声：一串垃圾（含孤立 0xEF、0xAA 但构不成帧） */
    buf[k++] = 0x12; buf[k++] = 0x34;
    buf[k++] = FM225_SYNC0; buf[k++] = 0x55;      /* 0xEF 后不是 0xAA：作废 */
    buf[k++] = 0x99;

    /* 坏帧：XOR 错 */
    const uint8_t d[] = { 0x12, 0x0D, 0x01 };
    uint8_t raw[64];
    size_t n = build_frame(raw, FM225_MSGID_REPLY, d, 3, NULL);
    raw[n - 1] ^= 0xFF;
    memcpy(buf + k, raw, n);
    k += n;

    /* 好帧：verify 成功 uid=1 —— 前面全是坏数据，这帧必须被解出 */
    const uint8_t d2[] = { 0x12, 0x00, 0x00, 0x01 };
    k += build_frame(buf + k, FM225_MSGID_REPLY, d2, 4, NULL);

    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    fm225_proto_feed(&ctx, buf, k);

    CHECK(r.n == 1);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_VERIFY);
    CHECK(r.frames[0].result == FM225_MR_SUCCESS);

    size_t frames, bad = 0, resync = 0;
    frames = fm225_proto_stats(&ctx, &bad, &resync, NULL);
    CHECK(frames == 1);
    CHECK(bad == 1);
    CHECK(resync >= 1);   /* 噪声/坏帧头错位至少触发一次重同步 */
}

/* ---------------- 用例 5：超时（半帧跨 tick 作废） ---------------- */

static void test_timeout(void)
{
    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    /* 喂半帧：EF AA 00 00 05 13 00（缺 Data 尾部与 XOR） */
    static const uint8_t half[] = { 0xEF, 0xAA, 0x00, 0x00, 0x05, 0x13, 0x00 };
    fm225_proto_feed(&ctx, half, sizeof(half));
    CHECK(r.n == 0);

    /* tick 推进 250ms（> 200ms 阈值）：半帧应作废 */
    fm225_proto_tick(&ctx, 250);
    CHECK(r.n == 0);

    size_t frames, to = 0;
    frames = fm225_proto_stats(&ctx, NULL, NULL, &to);
    CHECK(frames == 0);
    CHECK(to == 1);

    /* 作废后再喂完整帧：状态已复位，帧应正常解出 */
    const uint8_t d[] = { 0x12, 0x08 };
    uint8_t raw[64];
    size_t n = build_frame(raw, FM225_MSGID_REPLY, d, 2, NULL);
    fm225_proto_feed(&ctx, raw, n);
    CHECK(r.n == 1);
    CHECK(r.frames[0].mid_or_nid == FM225_CMD_VERIFY);

    /* 未超时的半帧不被误杀：喂半帧后 20ms 内 tick，再补齐剩余字节仍成帧 */
    recv_t r2;
    memset(&r2, 0, sizeof(r2));
    fm225_proto_ctx_t ctx2;
    fm225_proto_init(&ctx2, on_frame, &r2);

    fm225_proto_feed(&ctx2, half, sizeof(half));
    fm225_proto_tick(&ctx2, 20);           /* 20ms << 200ms：保留半帧 */
    const uint8_t tail[] = { 0x00, 0x03, 0x1F, 0x0A };   /* 补齐成手册示例帧 */
    fm225_proto_feed(&ctx2, tail, sizeof(tail));
    fm225_proto_tick(&ctx2, 40);
    CHECK(r2.n == 1);
    CHECK(r2.frames[0].mid_or_nid == FM225_CMD_ENROLL);

    size_t to2 = 0;
    fm225_proto_stats(&ctx2, NULL, NULL, &to2);
    CHECK(to2 == 0);   /* 半帧未被误杀 */
}

/* ---------------- 用例 6：超长帧丢弃（Size > FM225_MAX_DATA） ---------------- */

static void test_oversize_frame(void)
{
    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    /* 构造 Size=FM225_MAX_DATA+1 的帧（只喂到 Size 低字节即可触发丢弃） */
    static const uint8_t head[] = {
        0xEF, 0xAA, 0x00,
        (uint8_t)((FM225_MAX_DATA + 1) >> 8),
        (uint8_t)((FM225_MAX_DATA + 1) & 0xFF),
    };
    fm225_proto_feed(&ctx, head, sizeof(head));
    CHECK(r.n == 0);

    /* 状态已复位：接着喂好帧可解 */
    const uint8_t d[] = { 0x00 };
    uint8_t raw[64];
    size_t n = build_frame(raw, FM225_MSGID_NOTE, d, 1, NULL);
    fm225_proto_feed(&ctx, raw, n);
    CHECK(r.n == 1);
    CHECK(r.frames[0].mid_or_nid == FM225_NID_READY);
}

/* ---------------- 用例 7：空 Data 帧（Size=0） ---------------- */

static void test_empty_data_frame(void)
{
    /* H>>M 侧的 RESET 帧就是 Size=0（EF AA 10 00 00 10，手册示例）；
     * M>>H 不会有 Size=0 帧，但解析器应健壮处理（msgid/nid 取 0）。 */
    recv_t r;
    memset(&r, 0, sizeof(r));
    fm225_proto_ctx_t ctx;
    fm225_proto_init(&ctx, on_frame, &r);

    static const uint8_t reset[] = { 0xEF, 0xAA, 0x10, 0x00, 0x00, 0x10 };
    fm225_proto_feed(&ctx, reset, sizeof(reset));
    CHECK(r.n == 1);
    CHECK(r.frames[0].msgid == 0x10);        /* msgid 字段原样（非 REPLY/NOTE） */
    CHECK(r.frames[0].data_len == 0);
}

int main(void)
{
    test_manual_example_frame();
    test_half_packet();
    test_sticky_packets();
    test_bad_checksum();
    test_bad_checksum_resync_from_noise();
    test_timeout();
    test_oversize_frame();
    test_empty_data_frame();

    TEST_RESULT();
}
