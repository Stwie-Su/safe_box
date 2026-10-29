/**
 * @file hal_time.h
 * 取时间的统一入口。业务代码一律用它，不要直接调 time() / clock_gettime()。
 *
 * 这样以后换时间源（系统时间 → RTC 芯片）时，只改后端，别的代码不用动。
 * 两种时间不要混用：
 *   hal_time()    —— Unix 秒，会随校时变化，用于日志时间、动态码。断电会丢（除非有 RTC）。
 *   hal_time_ms() —— 开机以来的毫秒数，只会往前走，用于算超时、算间隔。
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIME_SRC_SYS = 0,   /* 系统时间（没有电池，断电就归零） */
    TIME_SRC_RTC,       /* 外部 RTC 芯片（有电池，断电也走得准） */
} time_src_t;

/* 当前 Unix 秒。 */
uint32_t hal_time(void);

/* 开机以来的毫秒数（不受校时影响）。算超时用它。 */
uint32_t hal_time_ms(void);

/* 校时。系统时间后端改系统时钟，RTC 后端写芯片。 */
safe_err_t hal_time_set(uint32_t unix_seconds);

/* 现在用的是哪个时间源。 */
time_src_t hal_time_source(void);

#ifdef __cplusplus
}
#endif
