/**
 * @file actuator_backend.h
 * 执行器后端接口（内部头文件，业务层不要包含）。
 */
#pragma once

#include "hal/hal_actuator.h"

typedef struct {
    const char * name;
    safe_err_t (*init)(void);
    /* 拉高 ms 毫秒后自动拉低；时长上限已由门面截断 */
    safe_err_t (*pulse)(uint32_t ms);
    /* 立即拉低 */
    safe_err_t (*release)(void);
} hal_actuator_backend_t;

/* 后端在脉冲结束/强制拉低时调用，用于同步门面状态 */
void hal_actuator_notify_low(void);
