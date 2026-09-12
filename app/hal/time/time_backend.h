/**
 * @file time_backend.h
 * 时间后端接口（内部头文件，业务层不要包含）。
 *
 * 唯一例外：编排层 app.c 为在启动早期完成「时间后端选择」而包含它，
 * 只调用 time_service_init()。其余模块一律只用 hal/hal_time.h 的公开接口，
 * 这样换后端（sys <-> rtc）时业务代码可以零改动。
 */
#pragma once

#include <stdbool.h>

#include "hal/hal_time.h"

typedef struct {
    const char * name;
    time_src_t   src;
    uint32_t   (*now)(void);
    safe_err_t (*set)(uint32_t unix_seconds);
} hal_time_backend_t;

/* 后端选择（实现在 time_service.c）：幂等，app_main() 开头调用一次；
 * 未显式调用的路径（如单测）在首次取时时懒选择，默认 sys。 */
void time_service_init(void);

/* RTC 后端（实现在 time_rtc.c）：设备节点与可用性探测。 */
const char * time_rtc_dev(void);
bool         time_rtc_available(void);
