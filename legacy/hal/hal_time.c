/**
 * @file hal_time.c
 * 时间基准抽象（阶段 1：系统时间 + 偏移量校时）。
 * 阶段 5 只需替换 hal_time() / hal_time_set() 内部实现为 DS3231 读写，
 * 其余业务代码（TOTP、日志时间戳）无需改动（NFR-5）。
 */
#include "hal_time.h"
#include <time.h>

static int64_t g_offset = 0;   /* 校时偏移：hal_time() = time(NULL) + g_offset */

uint32_t hal_time(void)
{
    return (uint32_t)((int64_t)time(NULL) + g_offset);
}

int hal_time_set(uint32_t t)
{
    g_offset = (int64_t)t - (int64_t)time(NULL);
    return 0;
}

time_src_t hal_time_source(void)
{
    return TIME_SRC_SYS;
}
