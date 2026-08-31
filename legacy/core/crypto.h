/**
 * @file crypto.h
 * 密码文件的认证加密层（工业级「安全存储凭据」方案）。
 *
 * 算法组合：AES-128-CBC + 随机 IV(CSPRNG) + PBKDF2-HMAC-SHA256 派生双密钥
 *           (enc/mac 各 16 字节) + HMAC 完整性校验 + PKCS7 填充。
 *
 * 自包含文件格式（写入 password.cfg 的二进制）：
 *   MAGIC(4) | IV(16) | SALT(16) | HMAC(32) | CIPHERTEXT(16 的倍数)
 *
 * 设计要点：
 *  - 随机 IV + 随机 SALT 保证「相同明文每次加密结果都不同」，杜绝密文比对泄密。
 *  - HMAC 覆盖 IV||CIPHERTEXT，提供完整性/真实性校验，抵御篡改与选择密文攻击。
 *  - enc/mac 密钥由 PBKDF2 从口令派生并分离，避免单密钥兼任两种用途。
 *
 * 依赖第三方库：third_party/aes (tiny-AES-c)、third_party/sha256 (B-Con)。
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define CRYPTO_BLOCKLEN   16
#define CRYPTO_KEYLEN     16
#define CRYPTO_SALTLEN    16
#define CRYPTO_IVLEN      16
#define CRYPTO_MACLEN     32
#define CRYPTO_PLAIN_MAX  64    /* 明文密码最大长度（不含末尾 \0） */
#define CRYPTO_BLOB_MAX   160   /* 加密后二进制最大字节数（含文件头） */

/* 认证加密：plain -> out（自包含二进制 blob）。返回写入 out 的字节数，失败返回 0。 */
size_t crypto_encrypt(const char *plain, uint8_t *out, size_t out_cap);

/* 认证解密：in/in_len -> out（null 结尾明文）。HMAC 校验失败或格式错误返回 false。 */
bool crypto_decrypt(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

/* PBKDF2-HMAC-SHA256（单 block，dklen <= 32）。供用户 PIN 哈希 / 密码派生复用。 */
void crypto_pbkdf2_sha256(const uint8_t *pw, size_t pwlen,
                          const uint8_t *salt, size_t saltlen,
                          uint32_t iter, uint8_t *dk, size_t dklen);
