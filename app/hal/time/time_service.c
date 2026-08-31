/**
 * @file time_service.c
 * 时间源门面：当前固定绑系统时间后端。
 *
 * DS3231 到货后：实现 time_rtc.c，把这里的默认后端换掉即可，
 * 上层（TOTP、日志、UI 时钟）无感知。
 */

#include "time_backend.h"

#include <time.h>

extern const hal_time_backend_t hal_time_backend_sys;

static const hal_time_backend_t * s_backend = &hal_time_backend_sys;

uint32_t hal_time(void)
{
    return s_backend->now();
}

uint32_t hal_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

safe_err_t hal_time_set(uint32_t unix_seconds)
{
    return s_backend->set(unix_seconds);
}

time_src_t hal_time_source(void)
{
    return s_backend->src;
}
