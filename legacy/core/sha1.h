/**
 * @file sha1.h
 * 自包含 SHA-1（RFC 3174）+ HMAC-SHA1（RFC 2104）实现。
 *
 * 为什么不用 OpenSSL：
 *   - 需求 NFR-5 要求 Ubuntu 与 i.MX6ULL 共用同一套业务代码，仅 HAL 不同；
 *     OpenSSL 在 Buildroot 交叉工具链下要么不带、要么体积大（>2MB），
 *     是阶段 5 上板的明确风险（需求文档 7.5 把 OpenSSL 标为"待确认"）。
 *   - TOTP 只需要 HMAC-SHA1 一个原语，代码量不到 150 行，自实现零依赖，
 *     且可以脱离 GUI 单独跑 RFC 6238 测试向量验证（见 tools/test_totp.c）。
 *
 * 算法特性：
 *   - 输入长度按位计数，支持 >2^32 bit 的极端情况（uint64_t 计数）。
 *   - 无 malloc、无静态大缓冲，可在无 MMU / 裸机环境使用。
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
    uint32_t h[5];              /* H0..H4 */
    uint64_t bitlen;            /* 已处理的消息总位数 */
    uint8_t  buf[SHA1_BLOCK_LEN];
    size_t   buflen;
} sha1_ctx_t;

void sha1_init(sha1_ctx_t * c);
void sha1_update(sha1_ctx_t * c, const void * data, size_t len);
void sha1_final(sha1_ctx_t * c, uint8_t out[SHA1_DIGEST_LEN]);

/* 便捷：一次性计算 */
void sha1_bytes(const void * data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);

/* HMAC-SHA1：key 任意长度（>64 字节先哈希再填充），输出 20 字节 */
void hmac_sha1(const void * key, size_t keylen,
               const void * msg, size_t msglen,
               uint8_t out[SHA1_DIGEST_LEN]);

#ifdef __cplusplus
} /*extern "C"*/
#endif
