/**
 * @file test_totp.c
 * TOTP 用例（测试计划 1.2）：窗口容忍、防重放、错误码。
 */
#include "test_util.h"

#include <string.h>

#include "core/auth/totp.h"

/* 测试密钥：合法 Base32（26 字符 → 16 字节）。期望码全部由 totp_at_time 相对
 * 同一时间戳生成，因此密钥内容无关紧要，只要是合法 Base32。 */
static const char * SECRET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

int main(void)
{
    char code[8] = {0};
    int64_t used = 0;

    /* Base32 解码基本功能 */
    uint8_t raw[32];
    int n = totp_base32_decode(SECRET, raw, sizeof(raw));
    CHECK(n == 16);
    /* 'A'..'H' 是 0..7，首字节由 A(00000)+B 高 3 位(000) 组成，必为 0x00 */
    CHECK(raw[0] == 0x00);

    /* 生成三个相邻窗口的码 */
    char cur[8] = {0}, prev[8] = {0}, next[8] = {0}, old[8] = {0};
    uint64_t now = 1756462800;          /* 任意固定时间戳 */
    CHECK(totp_at_time(SECRET, now, 6, cur, sizeof(cur)) == 0);
    CHECK(totp_at_time(SECRET, now - TOTP_PERIOD, 6, prev, sizeof(prev)) == 0);
    CHECK(totp_at_time(SECRET, now + TOTP_PERIOD, 6, next, sizeof(next)) == 0);
    CHECK(totp_at_time(SECRET, now - 2 * TOTP_PERIOD, 6, old, sizeof(old)) == 0);

    /* 当前窗口码可通过，回填时间片 */
    CHECK(totp_verify(SECRET, cur, now, 0, &used) == 0);
    CHECK(used == (int64_t)(now / TOTP_PERIOD));

    /* 上一窗口（-30s）容忍 */
    CHECK(totp_verify(SECRET, prev, now, 0, &used) == 0);

    /* 下一窗口（+30s）容忍 */
    CHECK(totp_verify(SECRET, next, now, 0, &used) == 0);

    /* 上上个窗口（-60s）拒绝 */
    CHECK(totp_verify(SECRET, old, now, 0, &used) == -1);

    /* 防重放：同一时间片第二次提交必须拒绝 */
    int64_t last = (int64_t)(now / TOTP_PERIOD);
    CHECK(totp_verify(SECRET, cur, now, last, &used) == -2);

    /* 防重放：更早的时间片也不允许（必须严格递增） */
    CHECK(totp_verify(SECRET, prev, now, last, &used) == -2);

    /* 错误的码 */
    char wrong[8] = "000000";
    if(strcmp(wrong, cur) == 0) strcpy(wrong, "000001");
    CHECK(totp_verify(SECRET, wrong, now, 0, &used) == -1);

    /* 用户输入格式错误按"码错误"处理（返回 -1 计失败次数）；
     * -3 只留给密钥/参数这类编程错误 */
    CHECK(totp_verify(SECRET, "12a456", now, 0, &used) == -1);   /* 非数字 */
    CHECK(totp_verify(SECRET, "12345", now, 0, &used) == -1);    /* 长度不足 */
    CHECK(totp_verify("!!不是base32!!", cur, now, 0, &used) == -3);

    /* 倒计时在 1..30 之间（整除时刻=刚进入新窗口，剩满 30s） */
    int rem = totp_remaining(now);
    CHECK(rem >= 1 && rem <= TOTP_PERIOD);

    /* 时间片递增后可再次使用 */
    uint64_t later = now + TOTP_PERIOD;
    char cur2[8] = {0};
    CHECK(totp_at_time(SECRET, later, 6, cur2, sizeof(cur2)) == 0);
    CHECK(totp_verify(SECRET, cur2, later, last, &used) == 0);
    CHECK(used == (int64_t)(later / TOTP_PERIOD));

    TEST_RESULT();
}
