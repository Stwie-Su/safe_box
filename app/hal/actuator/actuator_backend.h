/**
 * @file actuator_backend.h
 * 锁后端的函数表（内部头，业务层用 hal_actuator.h）。
 */
#pragma once

#include "hal/hal_actuator.h"

typedef struct {
    const char * name;
    safe_err_t (*init)(void);
    /* 拉高 ms 毫秒后自动拉低。时长上限由上层截断过，这里不用再管。 */
    safe_err_t (*pulse)(uint32_t ms);
    /* 马上拉低 */
    safe_err_t (*release)(void);
} hal_actuator_backend_t;

/* 后端在脉冲结束或强制拉低时调，让上层知道现在已经是低电平。 */
void hal_actuator_notify_low(void);
