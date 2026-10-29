/**
 * @file hal_actuator.h
 * 锁的控制：输出一个电平脉冲，把电磁锁吸开。
 *
 * 为什么限制最长 500ms：电磁锁长时间通电会发热烧线圈。
 * 这一层强制截断，调用方传 5000 也只会输出 500ms，是硬保护。
 *
 * 三种后端：模拟（只打印，PC 用）、GPIO（板上 sysfs 引脚）、空（没有硬件）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_ACTUATOR_PULSE_MAX_MS  500u

/* 初始化：选后端、打开引脚。开机调一次。 */
safe_err_t hal_actuator_init(void);

/* 开锁：拉高 ms 毫秒后自动拉低。**立刻返回**，不阻塞界面。
 * 内部起一条短线程负责到点拉低，所以传进来的时长会被夹到 500ms 以内。 */
safe_err_t hal_actuator_pulse(uint32_t ms);

/* 立刻拉低（异常恢复时用）。 */
safe_err_t hal_actuator_release(void);

/* 现在是不是高电平（界面状态灯用）。 */
bool hal_actuator_state(void);

#ifdef __cplusplus
}
#endif
