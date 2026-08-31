/**
 * @file hal_actuator.h
 * 执行器抽象（FR-8 / NFR-7）。
 *
 * 脉冲上限 500ms 由本层强制截断，调用方传多大都不会超。
 * 当前实现为模拟后端（打印 + 线程延时），阶段 3 换 GPIO 后端。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_ACTUATOR_PULSE_MAX_MS  500u

safe_err_t hal_actuator_init(void);

/* 输出开锁脉冲：拉高 ms 毫秒后自动拉低，非阻塞返回。 */
safe_err_t hal_actuator_pulse(uint32_t ms);

/* 立即拉低（异常恢复用）。 */
safe_err_t hal_actuator_release(void);

/* 当前是否处于高电平（供 UI 状态灯）。 */
bool hal_actuator_state(void);

#ifdef __cplusplus
}
#endif
