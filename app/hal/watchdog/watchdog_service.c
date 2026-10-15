/**
 * @file watchdog_service.c
 * 看门狗分发层：选后端、降级、幂等守卫、喂狗节流、转发。
 *
 * 后端选择：环境变量 SAFE_WATCHDOG_BACKEND（linux / null），默认 linux；
 * /dev/watchdog 打不开（PC、无狗设备、权限不足）时自动降级 null——保证系统在任何环境下都能起来，
 * 与 camera / time / face 模块一致的降级策略。
 *
 * 【为什么要节流】
 *   主循环 20ms 一拍，若每拍都真写一次 /dev/watchdog，等于每秒 50 次系统调用，纯浪费；
 *   而看门狗超时通常是秒级（默认 15s），因此分发层按 SAFE_WATCHDOG_KICK_MS（默认 1000ms）
 *   节流：不到间隔直接返回 OK，不打扰后端。卡死检测粒度因此是 1 秒，仍远小于 15 秒超时。
 */
#include "watchdog_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/hal_time.h"

extern const hal_watchdog_backend_t hal_watchdog_backend_linux;
extern const hal_watchdog_backend_t hal_watchdog_backend_none;

static const hal_watchdog_backend_t * s_backend = &hal_watchdog_backend_none;
static bool s_inited;
static bool s_started;
static uint32_t s_last_kick_ms;
static uint32_t s_min_interval_ms = 1000u;   /* 喂狗节流窗口，可由 SAFE_WATCHDOG_KICK_MS 覆盖 */

/* ------------------------------------------------------------------
 * 函数：find_backend（文件内私有）
 * 功能：按名字查后端；name 为 NULL 时返回表里第一个（表序即优先级）。
 * 调用者：仅 hal_watchdog_init()。
 * ------------------------------------------------------------------ */
static const hal_watchdog_backend_t * find_backend(const char * name)
{
    const hal_watchdog_backend_t * table[] = {
        &hal_watchdog_backend_linux,
        &hal_watchdog_backend_none,
    };
    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if(table[i] != NULL && (name == NULL || strcmp(table[i]->name, name) == 0)) return table[i];
    }
    return NULL;
}

safe_err_t hal_watchdog_init(uint32_t timeout_ms)
{
    if(s_inited) return SAFE_OK;

    const char * iv = getenv("SAFE_WATCHDOG_KICK_MS");
    if(iv != NULL && *iv != '\0') {
        unsigned long v = strtoul(iv, NULL, 10);
        if(v > 0) s_min_interval_ms = (uint32_t)v;
    }

    const char * want = getenv("SAFE_WATCHDOG_BACKEND");
    const hal_watchdog_backend_t * b = find_backend((want != NULL && *want != '\0') ? want : "linux");
    if(b == NULL) b = &hal_watchdog_backend_none;

    safe_err_t e = b->init(timeout_ms);
    if(e != SAFE_OK && b != &hal_watchdog_backend_none) {
        printf("[WATCHDOG] 后端 %s 初始化失败(%d)，降级 null（不喂真狗）\n", b->name, e);
        b = &hal_watchdog_backend_none;
        e = b->init(timeout_ms);
    }

    s_backend = b;
    s_started     = false;
    s_inited      = true;
    s_last_kick_ms = 0;
    printf("[WATCHDOG] 后端=%s timeout=%ums 节流=%ums\n", s_backend->name, timeout_ms, s_min_interval_ms);
    return e;
}

safe_err_t hal_watchdog_start(void)
{
    if(!s_inited) return SAFE_ERR_STATE;
    if(s_started) return SAFE_OK;
    safe_err_t e = s_backend->start();
    if(e == SAFE_OK) {
        s_started      = true;
        s_last_kick_ms = hal_time_ms();
    }
    return e;
}

safe_err_t hal_watchdog_kick(void)
{
    if(!s_inited || !s_started) return SAFE_ERR_STATE;

    uint32_t now = hal_time_ms();
    if(s_last_kick_ms != 0 && (now - s_last_kick_ms) < s_min_interval_ms) return SAFE_OK;   /* 节流窗口内不打扰后端 */
    s_last_kick_ms = now;
    return s_backend->kick();
}

safe_err_t hal_watchdog_stop(void)
{
    if(!s_inited) return SAFE_ERR_STATE;
    if(!s_started) return SAFE_OK;
    safe_err_t e = s_backend->stop();
    if(e == SAFE_OK) s_started = false;
    return e;
}

void hal_watchdog_deinit(void)
{
    if(!s_inited) return;
    s_backend->deinit();
    s_backend  = &hal_watchdog_backend_none;
    s_started  = false;
    s_inited   = false;
}

uint32_t hal_watchdog_kick_count(void)
{
    if(!s_inited || s_backend->kick_count == NULL) return 0;
    return s_backend->kick_count();
}
