/**
 * @file time_backend.h
 * 时间后端的函数表（内部头，业务层用 hal_time.h）。
 *
 * 唯一的例外：app.c 要在启动最前面选一次时间后端，所以它包含本文件、
 * 只调 time_service_init()。别的模块都用 hal_time.h 的接口，
 * 这样换后端（系统时间 ↔ RTC）时业务代码不用改。
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

/* 选后端。开机调一次，重复调也没关系。
 * 没调过的地方（比如单元测试）会在第一次取时间时自动选，默认用系统时间。 */
void time_service_init(void);

/* RTC 后端的设备节点和是否可用（诊断页显示用）。 */
const char * time_rtc_dev(void);
bool         time_rtc_available(void);
