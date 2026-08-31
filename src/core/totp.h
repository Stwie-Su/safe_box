/**
 * @file totp.h
 * RFC 6238 基于时间的一次性密码（TOTP）——纯 C 实现，零外部依赖。
 *
 * 关键参数（需求 NFR-3）：
 *   - 算法   HMAC-SHA1（自实现，见 sha1.h）
 *   - 步长   30 秒
 *   - 位数   6 位
 *   - 窗口   ±1（容忍前后各一个时间片，共 90 秒有效区间）
 *   - 防重放 同一时间片内一码一用，由调用方持久化 last_otp_counter
 *
 * 设计边界：本模块只做「密码学生成/比对」，不碰用户表、不落盘。
 * 与用户表和失败计数的耦合放在 core/unlock_backend.c。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOTP_PERIOD      30      /* 时间片长度（秒） */
#define TOTP_DIGITS      6       /* 产品使用的位数 */
#define TOTP_SECRET_B32  33      /* Base32 密钥缓冲区大小（16 字节 → 26 字符 + '\0'） */

/* Base32（RFC 4648 字符集 A-Z2-7）解码为字节。返回写入 out 的字节数，失败返回 -1。 */
int totp_base32_decode(const char * b32, uint8_t * out, size_t out_cap);

/* 生成随机密钥：16 字节熵 → Base32 字符串（26 字符 + '\0'）。0=成功。 */
int totp_secret_generate(char b32_out[TOTP_SECRET_B32]);

/* 计算 counter 时间片上的 TOTP。digits 取 6 或 8。out 需 ≥ digits+1。0=成功。 */
int totp_at(const char * b32, uint64_t counter, int digits, char * out, size_t out_cap);

/* 计算密钥在时间戳 t 上的 TOTP（等价 totp_at(b32, t / TOTP_PERIOD, ...)）。 */
int totp_at_time(const char * b32, uint64_t t, int digits, char * out, size_t out_cap);

/**
 * 校验 6 位动态码，带 ±1 窗口容忍与防重放。
 *
 * @param b32            用户 Base32 密钥
 * @param code           用户输入的 6 位码（纯数字）
 * @param now            当前 Unix 时间戳（用 hal_time()，便于将来切 RTC）
 * @param last_counter   上次成功使用过的时间片序号（0 = 从未使用）
 * @param used_counter   成功时回填本次使用的时间片序号
 * @return 0=通过；-1=码错误；-2=该时间片已用过（重放）；-3=参数/密钥非法
 *
 * 防重放语义：只有严格大于 last_counter 的时间片才会被接受。
 * 因此"同一窗口内第二次提交同一个码"必然返回 -2。
 */
int totp_verify(const char * b32, const char * code, uint64_t now,
                int64_t last_counter, int64_t * used_counter);

/* 距离当前时间片结束还剩多少秒（供 UI 画 30s 倒计时）。 */
int totp_remaining(uint64_t now);

#ifdef __cplusplus
} /*extern "C"*/
#endif
