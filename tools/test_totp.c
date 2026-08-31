/**
 * @file test_totp.c
 * 离线验证 totp.c / sha1.c 的 RFC 测试向量，不依赖 GUI / LVGL。
 *   编译：gcc test_totp.c ../src/core/totp.c ../src/core/sha1.c -o test_totp
 *   运行：./test_totp
 *
 * 覆盖：
 *   - RFC 2202 HMAC-SHA1 官方向量（含 key/msg 长度组合）
 *   - RFC 6238 Appendix B TOTP 向量（8 位，6 个时间点）
 *   - 防重放：同一时间片第二次提交必须失败
 *   - Base32 解码往返
 */
#include "../src/core/totp.h"
#include "../src/core/sha1.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
#define CHECK(cond, msg) do {                                            \
    if (cond) printf("  PASS  %s\n", msg);                              \
    else { printf("  FAIL  %s\n", msg); g_fail++; }                     \
} while (0)

/* 期望 20 字节摘要与给定十六进制串比对 */
static int sha_eq(const uint8_t * d, const char * hex)
{
    char buf[41];
    for (int i = 0; i < 20; i++) sprintf(buf + i * 2, "%02x", d[i]);
    return strcmp(buf, hex) == 0;
}

static void test_sha1(void)
{
    printf("[SHA-1]\n");
    uint8_t d[20];
    sha1_bytes("abc", 3, d);
    CHECK(sha_eq(d, "a9993e364706816aba3e25717850c26c9cd0d89d"), "\"abc\"");

    sha1_bytes("", 0, d);
    CHECK(sha_eq(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");

    const char * m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_bytes(m, strlen(m), d);
    CHECK(sha_eq(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "56-byte RFC vector");

    /* 多段 update 与一次性结果必须一致 */
    sha1_ctx_t c; uint8_t d2[20];
    sha1_init(&c);
    sha1_update(&c, "abcdb", 5);
    sha1_update(&c, "cdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", strlen(m) - 5);
    sha1_final(&c, d2);
    CHECK(sha_eq(d2, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "incremental update");
}

static void test_hmac(void)
{
    printf("[HMAC-SHA1 / RFC 2202]\n");
    uint8_t mac[20];

    /* 用例 1：key="Jefe", data="what do ya want for nothing?" */
    hmac_sha1("Jefe", 4, "what do ya want for nothing?", 28, mac);
    CHECK(sha_eq(mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "key=Jefe");

    /* 用例 2：key=0x0b*20, data="Hi There" */
    uint8_t k20[20]; memset(k20, 0x0b, sizeof(k20));
    hmac_sha1(k20, 20, "Hi There", 8, mac);
    CHECK(sha_eq(mac, "b617318655057264e28bc0b6fb378c8ef146be00"), "key=0x0b*20");

    /* 用例 3：key=0xaa*20, data=0xdd*50（RFC 2202 例 3） */
    uint8_t k20b[20]; memset(k20b, 0xaa, sizeof(k20b));
    uint8_t dd50[50]; memset(dd50, 0xdd, sizeof(dd50));
    hmac_sha1(k20b, 20, dd50, 50, mac);
    CHECK(sha_eq(mac, "125d7342b9ac11cd91a39af48aa17b4f63f175d3"), "key=0xaa*20, data=0xdd*50");

    /* 长密钥（>64 字节，需先哈希）分支：RFC 2202 例 6
     *   key=0xaa*80, data="Test Using Larger Than Block-Size Key - Hash Key First" */
    uint8_t k80[80]; memset(k80, 0xaa, sizeof(k80));
    hmac_sha1(k80, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, mac);
    CHECK(sha_eq(mac, "aa4ae5e15272d00e95705637ce8a3b55ed402112"), "long key (>64) hashed first");
}

static void test_base32(void)
{
    printf("[Base32]\n");
    uint8_t raw[64];
    int n = totp_base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", raw, sizeof(raw));
    const char * secret = "12345678901234567890";
    CHECK(n == 20 && memcmp(raw, secret, 20) == 0, "RFC6238 secret decodes (20 bytes)");
    CHECK(totp_base32_decode("ABCD", raw, 1) == -1, "out_cap too small -> -1");
}

static void test_totp_rfc6238(void)
{
    printf("[TOTP / RFC 6238 Appendix B, 8 digits]\n");
    const char * b32 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";   /* = "12345678901234567890" */
    struct { uint64_t t; const char * code; } v[6] = {
        { 59,            "94287082" },
        { 1111111109,    "07081804" },
        { 1111111111,    "14050471" },
        { 1234567890,    "89005924" },
        { 2000000000,    "69279037" },
        { 20000000000ULL,"65353130" },
    };
    for (int i = 0; i < 6; i++) {
        char code[16];
        totp_at_time(b32, v[i].t, 8, code, sizeof(code));
        char msg[64]; sprintf(msg, "T=%llu -> %s", (unsigned long long)v[i].t, v[i].code);
        CHECK(strcmp(code, v[i].code) == 0, msg);
    }
}

static void test_verify_and_replay(void)
{
    printf("[verify + anti-replay]\n");
    const char * b32 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    uint64_t now = 1111111109;
    int64_t last = 0, used = 0;

    char code[16];
    totp_at_time(b32, now, 6, code, sizeof(code));   /* 当前窗口码 */

    CHECK(totp_verify(b32, code, now, last, &used) == 0, "current window code passes");
    CHECK(used > last, "used_counter advances");

    /* 同一时间片再次提交同码 → 重放，必须 -2 */
    int64_t last2 = used;
    int r = totp_verify(b32, code, now, last2, &used);
    CHECK(r == -2, "same window reused -> -2 (replay)");

    /* 干净状态：±1 窗口容忍（上一窗口、下一窗口的码都应接受） */
    char code_prev[16], code_next[16];
    totp_at_time(b32, now - 30, 6, code_prev, sizeof(code_prev));
    totp_at_time(b32, now + 30, 6, code_next, sizeof(code_next));
    CHECK(totp_verify(b32, code_prev, now, 0, &used) == 0, "prev window (-30s) tolerated");
    CHECK(totp_verify(b32, code_next, now, 0, &used) == 0, "next window (+30s) tolerated");

    /* 错误码必须失败 */
    CHECK(totp_verify(b32, "000000", now, 0, &used) == -1, "wrong code -> -1");
}

int main(void)
{
    printf("======== TOTP / SHA1 self-test (RFC vectors) ========\n");
    test_sha1();
    test_hmac();
    test_base32();
    test_totp_rfc6238();
    test_verify_and_replay();
    printf("=====================================================\n");
    if (g_fail == 0) { printf("ALL PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
