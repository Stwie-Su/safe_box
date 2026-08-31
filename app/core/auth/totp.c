/**
 * @file totp.c
 * RFC 6238 TOTP 实现。依赖 core/sha1.c（HMAC-SHA1），不依赖 OpenSSL。
 *
 * 实现要点与踩坑记录：
 *   1) Base32 是「5 bit 一组」的位流，不是字符映射表拼接。逐字符累加到
 *      一个 uint32_t 缓冲、够 8 bit 就吐一个字节，才是正确写法。
 *   2) HOTP 的计数器 C 必须是 8 字节【大端】。写成小端会得到完全对不上、
 *      但看起来"也是 6 位数字"的错误结果，排查成本极高。
 *   3) 动态截断的偏移量取摘要最后一个字节的【低 4 位】(0..15)，
 *      取 DB 时要从 offset 连读 4 字节并清掉最高符号位 (& 0x7fffffff)。
 *   4) 防重放靠调用方持久化 last_counter，本模块只负责"必须严格大于"的比较。
 */
#include "core/auth/totp.h"
#include "core/support/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- Base32（RFC 4648） ---------------- */

int totp_base32_decode(const char * b32, uint8_t * out, size_t out_cap)
{
    if (!b32 || !out || out_cap == 0) return -1;

    uint32_t acc = 0;
    int      bits = 0;
    size_t   n = 0;

    for (const char * p = b32; *p; p++) {
        uint8_t v;
        char c = *p;
        if (c >= 'A' && c <= 'Z')      v = (uint8_t)(c - 'A');
        else if (c >= 'a' && c <= 'z') v = (uint8_t)(c - 'a');   /* 容忍小写输入 */
        else if (c >= '2' && c <= '7') v = (uint8_t)(c - '2' + 26);
        else if (c == '=')             break;                     /* 忽略填充 */
        else if (c == ' ' || c == '-') continue;                  /* 容忍分隔符 */
        else return -1;

        acc = (acc << 5) | v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return (int)n;
}

static const char B32_ALPHABET[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/* 字节流 → Base32 字符串（不补 '='）。返回写入字符数（不含 '\0'），失败返回 -1。 */
static int base32_encode(const uint8_t * in, size_t in_len, char * out, size_t out_cap)
{
    size_t need = (in_len * 8 + 4) / 5;
    if (!out || out_cap < need + 1) return -1;

    size_t o = 0;
    uint32_t acc = 0;
    int      bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[o++] = B32_ALPHABET[(acc >> bits) & 0x1Fu];
        }
    }
    if (bits > 0) out[o++] = B32_ALPHABET[(acc << (5 - bits)) & 0x1Fu];
    out[o] = '\0';
    return (int)o;
}

/* 随机字节：优先 /dev/urandom，失败退化为 rand()（此时安全性降级但功能可用） */
static void fill_random(uint8_t * buf, size_t len)
{
    FILE * f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t rd = fread(buf, 1, len, f);
        fclose(f);
        if (rd == len) return;
    }
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&f);
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rand() & 0xFF);
}

int totp_secret_generate(char b32_out[TOTP_SECRET_B32])
{
    if (!b32_out) return -1;
    uint8_t raw[16];
    fill_random(raw, sizeof(raw));
    if (base32_encode(raw, sizeof(raw), b32_out, TOTP_SECRET_B32) < 0) return -1;
    return 0;
}

/* ---------------- HOTP / TOTP ---------------- */

/* HOTP(K, C)：C 为 8 字节大端计数器；digits 位动态截断结果写 out。 */
static int hotp(const uint8_t * key, size_t keylen, uint64_t counter, int digits,
                char * out, size_t out_cap)
{
    if (digits < 6 || digits > 8 || !out || out_cap < (size_t)digits + 1) return -1;

    /* 计数器必须是 8 字节【大端】；写成小端会得到"看着也像 6 位数"的错值 */
    uint8_t msg[8];
    for (int i = 0; i < 8; i++) msg[7 - i] = (uint8_t)((counter >> (8 * i)) & 0xFFu);

    uint8_t mac[SHA1_DIGEST_LEN];
    hmac_sha1(key, keylen, msg, sizeof(msg), mac);

    int offset = mac[SHA1_DIGEST_LEN - 1] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[offset] & 0x7F) << 24) |
                   ((uint32_t)mac[offset + 1] << 16) |
                   ((uint32_t)mac[offset + 2] << 8) |
                   ((uint32_t)mac[offset + 3]);

    static const uint32_t POW10[9] = { 1, 10, 100, 1000, 10000,
                                       100000, 1000000, 10000000, 100000000 };
    uint32_t code = bin % POW10[digits];

    for (int i = digits - 1; i >= 0; i--) {
        out[i] = (char)('0' + (code % 10));
        code /= 10;
    }
    out[digits] = '\0';
    return 0;
}

int totp_at(const char * b32, uint64_t counter, int digits, char * out, size_t out_cap)
{
    uint8_t key[64];
    int klen = totp_base32_decode(b32, key, sizeof(key));
    if (klen <= 0) return -1;
    return hotp(key, (size_t)klen, counter, digits, out, out_cap);
}

int totp_at_time(const char * b32, uint64_t t, int digits, char * out, size_t out_cap)
{
    return totp_at(b32, t / TOTP_PERIOD, digits, out, out_cap);
}

int totp_verify(const char * b32, const char * code, uint64_t now,
                int64_t last_counter, int64_t * used_counter)
{
    if (!b32 || !*b32 || !code) return -3;

    size_t clen = strlen(code);
    if (clen != (size_t)TOTP_DIGITS) return -1;
    for (size_t i = 0; i < clen; i++) {
        if (code[i] < '0' || code[i] > '9') return -1;
    }

    uint8_t key[64];
    int klen = totp_base32_decode(b32, key, sizeof(key));
    if (klen <= 0) return -3;

    int64_t t = (int64_t)(now / TOTP_PERIOD);
    /* ±1 窗口：先试当前，再试前后各一片。
     * 关键：先按 counter 找匹配，再判断该 counter 是否已被用过。
     * 不能简单地用 continue 跳过 c<=last_counter，否则"同码重放"会被误判成
     * "码错误"（-1），失去防重放语义。 */
    const int64_t order[3] = { t, t - 1, t + 1 };
    for (int i = 0; i < 3; i++) {
        int64_t c = order[i];
        if (c < 0) continue;
        char expect[TOTP_DIGITS + 1];
        if (hotp(key, (size_t)klen, (uint64_t)c, TOTP_DIGITS, expect, sizeof(expect)) != 0) return -3;
        if (memcmp(expect, code, TOTP_DIGITS) != 0) continue;
        if (c <= last_counter) return -2;          /* 该时间片已用过 → 重放 */
        if (used_counter) *used_counter = c;
        return 0;
    }
    return -1;
}

int totp_remaining(uint64_t now)
{
    return (int)(TOTP_PERIOD - (now % TOTP_PERIOD));
}
