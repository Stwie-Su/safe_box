/**
 * @file time_backend.h
 * 时间后端接口（内部头文件，业务层不要包含）。
 */
#pragma once

#include "hal/hal_time.h"

typedef struct {
    const char * name;
    time_src_t   src;
    uint32_t   (*now)(void);
    safe_err_t (*set)(uint32_t unix_seconds);
} hal_time_backend_t;
