/**
 * @file totp.h
 * 动态码（TOTP）算法：按时间算出一个 6 位数字，和手机上的验证器一样。
 *
 * 参数：HMAC-SHA1，30 秒换一次，6 位数字，允许前后各一个时间片（共 90 秒有效）。
 * 防重放：同一个时间片的码只能用一次，用过的记录下来（调用方负责存）。
 *
 * 本文件只做数学计算，不读用户表、不写文件；和用户的关联在 auth_verify.c 里做。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOTP_PERIOD      30      /* 多少秒换一次码 */
#define TOTP_DIGITS      6       /* 几位数字 */
#define TOTP_SECRET_B32  33      /* 密钥字符串缓冲（16 字节 → 26 个字符 + '\0'） */

/* Base32 解码。返回解出的字节数，失败返回 -1。 */
int totp_base32_decode(const char * b32, uint8_t * out, size_t out_cap);

/* 随机生成一个密钥，输出 Base32 字符串（26 字符）。0 = 成功。 */
int totp_secret_generate(char b32_out[TOTP_SECRET_B32]);

/* 算第 counter 个时间片的码。digits 只能是 6 或 8；out 至少 digits+1 字节。0 = 成功。 */
int totp_at(const char * b32, uint64_t counter, int digits, char * out, size_t out_cap);

/* 算某个时间戳对应的码（内部就是 t / 30）。 */
int totp_at_time(const char * b32, uint64_t t, int digits, char * out, size_t out_cap);

/* 校验用户输入的码。
 * 返回 0 = 通过；-1 = 码不对；-2 = 这个时间片已经用过了；-3 = 参数或密钥有问题。
 * last_counter 是上次成功用的时间片（0 = 从没用过），成功时把这次的时间片写进 used_counter。 */
int totp_verify(const char * b32, const char * code, uint64_t now,
                int64_t last_counter, int64_t * used_counter);

/* 当前这个 30 秒还剩几秒（界面画倒计时用）。 */
int totp_remaining(uint64_t now);

#ifdef __cplusplus
} /*extern "C"*/
#endif
