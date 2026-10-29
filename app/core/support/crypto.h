/**
 * @file crypto.h
 * 加密保存一份短文本（主要是 WiFi 密码），带防篡改校验。
 *
 * 用了什么：AES-128-CBC 加密 + 随机 IV / 随机盐 + PBKDF2 派生两把密钥
 * （一把加密、一把做校验）+ HMAC 校验 + PKCS7 填充。
 *
 * 为什么要随机 IV 和随机盐：同一个明文每次加密出来的结果都不同，
 * 别人拿到密文也没法靠「比对两次结果是否相同」猜内容。
 * HMAC 覆盖 IV 和密文，数据被改过会直接解不出来，不会解出一段乱码当结果用。
 *
 * 存下来的文件（password.cfg）长这样：
 *   MAGIC(4) | IV(16) | SALT(16) | HMAC(32) | 密文（16 的整数倍）
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
#define CRYPTO_PLAIN_MAX  64    /* 明文最长多少字节（不含结尾 \0） */
#define CRYPTO_BLOB_MAX   160   /* 加密后最大多少字节（含文件头） */

/* 加密：plain 写进 out，返回写入的字节数，失败返回 0。 */
size_t crypto_encrypt(const char *plain, uint8_t *out, size_t out_cap);

/* 解密：in 解到 out（自动补 '\0'）。校验不过或格式不对返回 false。 */
bool crypto_decrypt(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

/* PBKDF2-HMAC-SHA256（只算一个 block，dklen <= 32）。
 * 计算 PIN 的哈希、派生密钥都用它。 */
void crypto_pbkdf2_sha256(const uint8_t *pw, size_t pwlen,
                          const uint8_t *salt, size_t saltlen,
                          uint32_t iter, uint8_t *dk, size_t dklen);
