/**
 * @file sha1.h
 * 自己实现的 SHA-1 和 HMAC-SHA1，用来算动态码（TOTP）。
 *
 * 为什么不用现成的加密库：板子的交叉编译环境里不一定有，体积也大；
 * 而动态码只需要 HMAC-SHA1 这一个算法，一百多行就能写完，
 * 还能脱离界面、单独跑标准测试数据验证对不对。
 *
 * 特点：不用 malloc、没有大块静态缓冲，长度用 64 位计数，超长数据也不会算错。
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA1_DIGEST_LEN 20
#define SHA1_BLOCK_LEN  64

typedef struct {
    uint32_t h[5];              /* 五个中间状态 */
    uint64_t bitlen;            /* 已经处理了多少位 */
    uint8_t  buf[SHA1_BLOCK_LEN];
    size_t   buflen;
} sha1_ctx_t;

/* 三步算完：init → 可以多次 update → final 拿到 20 字节结果。 */
void sha1_init(sha1_ctx_t * c);
void sha1_update(sha1_ctx_t * c, const void * data, size_t len);
void sha1_final(sha1_ctx_t * c, uint8_t out[SHA1_DIGEST_LEN]);

/* 一次性算完（内部就是上面三步）。 */
void sha1_bytes(const void * data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);

/* HMAC-SHA1：key 长度随意（超过 64 字节会先哈希一遍），输出 20 字节。 */
void hmac_sha1(const void * key, size_t keylen,
               const void * msg, size_t msglen,
               uint8_t out[SHA1_DIGEST_LEN]);

#ifdef __cplusplus
} /*extern "C"*/
#endif
