/**
 * @file hal_time.h
 * 时间源抽象（FR-6 / NFR-5）。
 *
 * 业务层一律走本接口，不直接 <time.h> 取时：
 *   - 当前实现 = 系统时间（time_sys.c）；
 *   - DS3231 到货后新增 time_rtc.c 后端并切换，业务代码零改动。
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIME_SRC_SYS = 0,   /* 系统时间（无备份电池，断电丢失） */
    TIME_SRC_RTC,       /* 外部 RTC（断电保持） */
} time_src_t;

/* Unix 秒。TOTP 与日志时间戳用。 */
uint32_t hal_time(void);

/* 单调毫秒计数，用于 tick 与超时判定（不受校时影响）。 */
uint32_t hal_time_ms(void);

/* 校时：系统时间后端走 settimeofday，RTC 后端写芯片。 */
safe_err_t hal_time_set(uint32_t unix_seconds);

time_src_t hal_time_source(void);

#ifdef __cplusplus
}
#endif
