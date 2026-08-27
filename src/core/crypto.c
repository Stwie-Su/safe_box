/**
 * @file crypto.c
 * 认证加密实现：AES-128-CBC + 随机 IV + PBKDF2-HMAC-SHA256 + HMAC + PKCS7。
 * 详见 crypto.h 的文件头注释。
 */
#define _CRT_RAND_S            /* 在 include stdlib.h 前定义，暴露 Windows rand_s */
#include <stdlib.h>
#include <stdio.h>   /* Linux 分支用 FILE/fopen/fread/fclose（MSVC 下被 stdlib 间接包含，gcc 需显式） */
#include <string.h>
#include "crypto.h"
#include "aes.h"
#include "sha256.h"

/* 编译期口令：演示用固定口令，配合随机 SALT+IV 已足以抵御「看文件 / 抓串口」类风险。 */
static const char *BUILTIN_PASSPHRASE = "lvgl-safe-demo-pp-v1";
#define PBKDF2_ITER 10000u

/* ----------------------------- CSPRNG ----------------------------- */
/* 跨平台安全随机源：Windows 用 rand_s（CSPRNG），Linux 用 /dev/urandom。 */
static void get_random(uint8_t *buf, size_t len)
{
#ifdef _WIN32
    for (size_t i = 0; i < len; i += sizeof(unsigned int)) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) r = (unsigned int)rand();   /* 极端回退 */
        size_t n = (len - i < sizeof(r)) ? (len - i) : sizeof(r);
        memcpy(buf + i, &r, n);
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        (void)fread(buf, 1, len, f);
        fclose(f);
    } else {
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rand();
    }
#endif
}

/* --------------------------- HMAC-SHA256 -------------------------- */
static void hmac_sha256(const uint8_t *key, size_t klen,
                        const uint8_t *msg, size_t mlen,
                        uint8_t mac[SHA256_BLOCK_SIZE])
{
    uint8_t k0[64];
    if (klen > 64) {
        SHA256_CTX c; sha256_init(&c); sha256_update(&c, key, klen); sha256_final(&c, k0);
    } else {
        memcpy(k0, key, klen);
        memset(k0 + klen, 0, 64 - klen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    SHA256_CTX c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen);
    uint8_t inner[SHA256_BLOCK_SIZE]; sha256_final(&c, inner);

    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, SHA256_BLOCK_SIZE);
    sha256_final(&c, mac);
}

/* ------------------------ PBKDF2-HMAC-SHA256 ---------------------- */
/* 单 block 实现（dklen <= 32）。T = U1 ^ U2 ^ ... ^ Uc。 */
static void pbkdf2(const uint8_t *pw, size_t pwlen,
                   const uint8_t *salt, size_t saltlen,
                   uint32_t iter, uint8_t *dk, size_t dklen)
{
    uint8_t saltblk[16 + 4];                 /* salt || 32-bit block index(=1) */
    size_t sb_len = saltlen + 4;
    if (sb_len > sizeof(saltblk)) sb_len = sizeof(saltblk);
    memcpy(saltblk, salt, saltlen);
    saltblk[saltlen] = 0; saltblk[saltlen + 1] = 0;
    saltblk[saltlen + 2] = 0; saltblk[saltlen + 3] = 1;

    uint8_t U[SHA256_BLOCK_SIZE];
    hmac_sha256(pw, pwlen, saltblk, sb_len, U);
    uint8_t T[SHA256_BLOCK_SIZE];
    memcpy(T, U, SHA256_BLOCK_SIZE);
    for (uint32_t i = 1; i < iter; i++) {
        hmac_sha256(pw, pwlen, U, SHA256_BLOCK_SIZE, U);
        for (int j = 0; j < (int)SHA256_BLOCK_SIZE; j++) T[j] ^= U[j];
    }
    memcpy(dk, T, dklen);
}

/* ----------------------------- PKCS7 ----------------------------- */
static size_t pkcs7_pad(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    size_t pad = CRYPTO_BLOCKLEN - (in_len % CRYPTO_BLOCKLEN);
    if (in_len + pad > out_cap) return 0;
    memcpy(out, in, in_len);
    memset(out + in_len, (int)pad, pad);
    return in_len + pad;
}

static size_t pkcs7_unpad(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    if (in_len == 0 || in_len % CRYPTO_BLOCKLEN != 0) return 0;
    uint8_t pad = in[in_len - 1];
    if (pad < 1 || pad > CRYPTO_BLOCKLEN) return 0;
    for (size_t i = in_len - pad; i < in_len; i++) {
        if (in[i] != pad) return 0;
    }
    if (in_len - pad > out_cap) return 0;
    memcpy(out, in, in_len - pad);
    return in_len - pad;
}

/* ----------------------- 口令 -> 双密钥派生 ---------------------- */
static void derive_keys(const uint8_t *salt,
                        uint8_t enc_key[CRYPTO_KEYLEN],
                        uint8_t mac_key[CRYPTO_KEYLEN])
{
    uint8_t dk[32];
    pbkdf2((const uint8_t *)BUILTIN_PASSPHRASE, strlen(BUILTIN_PASSPHRASE),
           salt, CRYPTO_SALTLEN, PBKDF2_ITER, dk, 32);
    memcpy(enc_key, dk, CRYPTO_KEYLEN);
    memcpy(mac_key, dk + CRYPTO_KEYLEN, CRYPTO_KEYLEN);
}

/* ----------------------------- 加密 ------------------------------ */
size_t crypto_encrypt(const char *plain, uint8_t *out, size_t out_cap)
{
    size_t plen = strlen(plain);
    if (plen == 0 || plen > CRYPTO_PLAIN_MAX) return 0;

    uint8_t pt[CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN];
    size_t padded = pkcs7_pad((const uint8_t *)plain, plen, pt, sizeof(pt));
    if (padded == 0) return 0;

    if (4 + CRYPTO_IVLEN + CRYPTO_SALTLEN + CRYPTO_MACLEN + padded > out_cap) return 0;

    uint8_t iv[CRYPTO_IVLEN], salt[CRYPTO_SALTLEN];
    get_random(iv, CRYPTO_IVLEN);
    get_random(salt, CRYPTO_SALTLEN);

    uint8_t enc_key[CRYPTO_KEYLEN], mac_key[CRYPTO_KEYLEN];
    derive_keys(salt, enc_key, mac_key);

    uint8_t ct[CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN];
    memcpy(ct, pt, padded);
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, enc_key, iv);
    AES_CBC_encrypt_buffer(&ctx, ct, padded);

    size_t off = 0;
    memcpy(out + off, "SFP1", 4); off += 4;
    memcpy(out + off, iv, CRYPTO_IVLEN); off += CRYPTO_IVLEN;
    memcpy(out + off, salt, CRYPTO_SALTLEN); off += CRYPTO_SALTLEN;

    uint8_t macdata[CRYPTO_IVLEN + (CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN)];
    memcpy(macdata, iv, CRYPTO_IVLEN);
    memcpy(macdata + CRYPTO_IVLEN, ct, padded);
    hmac_sha256(mac_key, CRYPTO_KEYLEN, macdata, CRYPTO_IVLEN + padded, out + off);
    off += CRYPTO_MACLEN;
    memcpy(out + off, ct, padded); off += padded;
    return off;
}

/* ----------------------------- 解密 ------------------------------ */
bool crypto_decrypt(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    const size_t header = 4 + CRYPTO_IVLEN + CRYPTO_SALTLEN + CRYPTO_MACLEN;
    if (in_len < header) return false;
    if (memcmp(in, "SFP1", 4) != 0) return false;

    const uint8_t *iv   = in + 4;
    const uint8_t *salt = in + 4 + CRYPTO_IVLEN;
    const uint8_t *mac  = in + 4 + CRYPTO_IVLEN + CRYPTO_SALTLEN;
    const uint8_t *ct   = in + header;
    size_t ct_len = in_len - header;
    if (ct_len == 0 || ct_len % CRYPTO_BLOCKLEN != 0) return false;

    uint8_t enc_key[CRYPTO_KEYLEN], mac_key[CRYPTO_KEYLEN];
    derive_keys(salt, enc_key, mac_key);

    /* 先校验 HMAC（覆盖 IV||CIPHERTEXT），失败直接拒绝，避免解密被篡改数据 */
    uint8_t macdata[CRYPTO_IVLEN + (CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN)];
    memcpy(macdata, iv, CRYPTO_IVLEN);
    memcpy(macdata + CRYPTO_IVLEN, ct, ct_len);
    uint8_t expected[CRYPTO_MACLEN];
    hmac_sha256(mac_key, CRYPTO_KEYLEN, macdata, CRYPTO_IVLEN + ct_len, expected);
    if (memcmp(expected, mac, CRYPTO_MACLEN) != 0) return false;

    uint8_t pt[CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN];
    memcpy(pt, ct, ct_len);
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, enc_key, iv);
    AES_CBC_decrypt_buffer(&ctx, pt, ct_len);

    uint8_t plain[CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN];
    size_t plen = pkcs7_unpad(pt, ct_len, plain, sizeof(plain));
    if (plen == 0 || plen + 1 > out_cap) return false;
    memcpy(out, plain, plen);
    out[plen] = '\0';
    return true;
}

/* 公开包装：PBKDF2-HMAC-SHA256（单 block，dklen <= 32）。 */
void crypto_pbkdf2_sha256(const uint8_t *pw, size_t pwlen,
                          const uint8_t *salt, size_t saltlen,
                          uint32_t iter, uint8_t *dk, size_t dklen)
{
    pbkdf2(pw, pwlen, salt, saltlen, iter, dk, dklen);
}
