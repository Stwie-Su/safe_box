/**
 * @file sha1.c
 * SHA-1（RFC 3174）与 HMAC-SHA1（RFC 2104）实现。见 sha1.h 顶部"为什么不用 OpenSSL"。
 *
 * 实现要点（与常见错误对照）：
 *   1) 消息填充：追加 0x80，补 0 到长度 ≡ 56 (mod 64)，最后 8 字节大端位长度。
 *   2) W[16..79] 必须循环左移 1 位后展开：W[i] = ROTL(W[i-3]^W[i-8]^W[i-14]^W[i-16], 1)。
 *   3) 位长度用 uint64_t 累加，避免 >512MB 消息时 32 位溢出。
 */
#include "sha1.h"
#include <string.h>

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                       k = 0xCA62C1D6u; }

        uint32_t tmp = ROTL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROTL32(b, 30);
        b = a;
        a = tmp;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_ctx_t * c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha1_update(sha1_ctx_t * c, const void * data, size_t len)
{
    const uint8_t * p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;

    while (len > 0) {
        size_t need = SHA1_BLOCK_LEN - c->buflen;
        size_t take = (len < need) ? len : need;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == SHA1_BLOCK_LEN) {
            sha1_compress(c->h, c->buf);
            c->buflen = 0;
        }
    }
}

void sha1_final(sha1_ctx_t * c, uint8_t out[SHA1_DIGEST_LEN])
{
    /* 填充：0x80 || 0x00... || 8 字节大端位长度。
     * ★ 必须手工往 c->buf 里塞，不能借 sha1_update()：后者会累加 bitlen，
     *   把填充自身的长度也算进消息长度，结果恒错。 */
    uint64_t bits = c->bitlen;

    c->buf[c->buflen++] = 0x80;
    if (c->buflen > SHA1_BLOCK_LEN - 8) {      /* 这一块放不下长度字段，先压缩 */
        while (c->buflen < SHA1_BLOCK_LEN) c->buf[c->buflen++] = 0x00;
        sha1_compress(c->h, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < SHA1_BLOCK_LEN - 8) c->buf[c->buflen++] = 0x00;
    for (int i = 0; i < 8; i++) {
        c->buf[SHA1_BLOCK_LEN - 8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_compress(c->h, c->buf);
    c->buflen = 0;

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }

    /* 清空中间态，防止调用方复用已 finalize 的上下文 */
    memset(c, 0, sizeof(*c));
}

void sha1_bytes(const void * data, size_t len, uint8_t out[SHA1_DIGEST_LEN])
{
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

void hmac_sha1(const void * key, size_t keylen,
               const void * msg, size_t msglen,
               uint8_t out[SHA1_DIGEST_LEN])
{
    uint8_t ipad[SHA1_BLOCK_LEN], opad[SHA1_BLOCK_LEN];
    uint8_t k[SHA1_DIGEST_LEN];
    const uint8_t * kp = (const uint8_t *)key;
    size_t klen = keylen;

    /* 长密钥先哈希压缩到 20 字节（RFC 2104 §2） */
    if (klen > SHA1_BLOCK_LEN) {
        sha1_bytes(kp, klen, k);
        kp = k;
        klen = SHA1_DIGEST_LEN;
    }

    for (size_t i = 0; i < SHA1_BLOCK_LEN; i++) {
        uint8_t kb = (i < klen) ? kp[i] : 0;
        ipad[i] = (uint8_t)(kb ^ 0x36);
        opad[i] = (uint8_t)(kb ^ 0x5C);
    }

    sha1_ctx_t c;
    uint8_t inner[SHA1_DIGEST_LEN];
    sha1_init(&c);
    sha1_update(&c, ipad, SHA1_BLOCK_LEN);
    sha1_update(&c, msg, msglen);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, SHA1_BLOCK_LEN);
    sha1_update(&c, inner, SHA1_DIGEST_LEN);
    sha1_final(&c, out);

    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}
