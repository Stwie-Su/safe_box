/**
 * @file watchdog_backend.h
 * 看门狗后端的函数表（内部头，业务层用 hal_watchdog.h）。
 *
 * 两个实现：真的 /dev/watchdog，以及空实现（PC 上没有这个设备）。
 * ★ 下面成员的顺序必须和定义完全一致：C 语言按位置初始化，
 *   顺序错了不会报错，但会调到错的函数上。
 */
#pragma once

#include "hal/hal_watchdog.h"

typedef struct {
    const char * name;                      /* 后端名，用 SAFE_WATCHDOG_BACKEND 选 */
    safe_err_t (*init)(uint32_t timeout_ms);/* 打开设备、设超时（还不喂） */
    safe_err_t (*start)(void);              /* 开始计时，立刻喂第一口 */
    safe_err_t (*kick)(void);               /* 喂一次 */
    safe_err_t (*stop)(void);               /* 停止计时（关掉前必须做） */
    void       (*deinit)(void);             /* 关设备 */
    uint32_t   (*kick_count)(void);         /* 喂了多少次 */
} hal_watchdog_backend_t;
