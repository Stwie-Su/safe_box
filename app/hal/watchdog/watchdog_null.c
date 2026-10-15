/**
 * @file watchdog_null.c
 * 看门狗空对象：PC / 无 /dev/watchdog 的设备上的占位实现。
 *
 * 「什么都不做」也是一种实现：上层照常 init/start/kick/stop，只是不产生复位能力。
 * 这样同一份主循环代码（含喂狗心跳）在 PC 与板子上都能编译运行，不必到处写
 * `#ifdef BOARD` 或「有没有狗」的分支。
 *
 * kick 计数照常递增——用于自测与状态显示（「喂狗心跳是否还在跑」这一点在 null 后端也能验证）。
 */
#include "watchdog_backend.h"

#include <stdbool.h>
#include <stdio.h>

static uint32_t s_kicks;
static bool     s_started;

static safe_err_t null_init(uint32_t timeout_ms)
{
    printf("[WATCHDOG] null 后端：不喂真狗（timeout=%ums 仅记录）\n", timeout_ms);
    return SAFE_OK;
}

static safe_err_t null_start(void)
{
    s_started = true;
    return SAFE_OK;
}

static safe_err_t null_kick(void)
{
    if(!s_started) return SAFE_ERR_STATE;
    s_kicks++;
    return SAFE_OK;
}

static safe_err_t null_stop(void)
{
    s_started = false;
    return SAFE_OK;
}

static void null_deinit(void)
{
    s_started = false;
}

static uint32_t null_kick_count(void)
{
    return s_kicks;
}

const hal_watchdog_backend_t hal_watchdog_backend_null = {
    "null",
    null_init,
    null_start,
    null_kick,
    null_stop,
    null_deinit,
    null_kick_count,
};
