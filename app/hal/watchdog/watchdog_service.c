/**
 * @file watchdog_service.c
 * 看门狗分发层：选后端、降级、幂等守卫、喂狗节流、转发。
 *
 * 后端选择：
 *   - 显式指定 SAFE_WATCHDOG_BACKEND（linux / file / null）时只试该后端，失败降级 null；
 *   - 未指定时按 **linux → file → null** 自动降级：
 *       linux = /dev/watchdog 真狗（能复位整机）
 *       file  = 软件狗（写心跳文件，由 supervisor --heartbeat 检查）
 *       null  = 占位（PC / 无权限写 /var/run）
 *   ★ 软狗不是硬件狗的等价替代：它只能让 supervisor 发现「进程卡死并重启」，
 *     不能在内核 panic / 整机死机时复位设备。边界见 backend_file.c 头注释。
 *
 * 【两层同时生效（伴随层）】
 *   真狗和软狗解决的是不同粒度的问题，不该二选一：
 *     真狗 = 复位整机，代价大（重启 30s+、丢现场），但内核 panic / 整机死机也能救；
 *     软狗 = 让 supervisor 只重启进程（保留日志、不黑屏重来），处理「进程还在但主循环卡死」。
 *   所以主后端选中 linux 时，若显式设置了 SAFE_WATCHDOG_FILE，再挂一层 file 作为
 *   **伴随层**，与主后端一起被 kick / start / stop。
 *   响应顺序因此是：5s 软狗先手重启进程 → 进程若起不来，60s 真狗兜底复位整机。
 *   未设置 SAFE_WATCHDOG_FILE 时不启用伴随层（PC 上 /var/run 不可写，避免噪声日志）。
 *
 * 【为什么要节流】
 *   主循环 20ms 一拍，若每拍都真写一次 /dev/watchdog，等于每秒 50 次系统调用，纯浪费；
 *   而看门狗超时是秒级（默认 60s，见 hal_watchdog.h），因此分发层按
 *   SAFE_WATCHDOG_KICK_MS（默认 1000ms）节流：不到间隔直接返回 OK，不打扰后端。
 *   卡死检测粒度因此是 1 秒，远小于 60 秒超时，不会误判。
 */
#include "watchdog_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/hal_time.h"

extern const hal_watchdog_backend_t hal_watchdog_backend_linux;
extern const hal_watchdog_backend_t hal_watchdog_backend_file;
extern const hal_watchdog_backend_t hal_watchdog_backend_none;

static const hal_watchdog_backend_t * s_backend   = &hal_watchdog_backend_none;
static const hal_watchdog_backend_t * s_companion = NULL;   /* 软狗伴随层（与主后端并行） */
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
        &hal_watchdog_backend_file,
        &hal_watchdog_backend_none,
    };
    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if(table[i] != NULL && (name == NULL || strcmp(table[i]->name, name) == 0)) return table[i];
    }
    return NULL;
}

/* 依次尝试自动降级链；返回实际生效的后端（none 兜底）。 */
static const hal_watchdog_backend_t * pick_backend(uint32_t timeout_ms)
{
    const hal_watchdog_backend_t * chain[] = {
        &hal_watchdog_backend_linux,
        &hal_watchdog_backend_file,
        &hal_watchdog_backend_none,
    };

    for(size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        safe_err_t e = chain[i]->init(timeout_ms);
        if(e == SAFE_OK) return chain[i];
        if(chain[i] != &hal_watchdog_backend_none) {
            printf("[WATCHDOG] 后端 %s 不可用(%d)，尝试下一个\n", chain[i]->name, e);
        }
    }
    return &hal_watchdog_backend_none;   /* 理论上不可达：null 的 init 恒成功 */
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
    const hal_watchdog_backend_t * b = NULL;
    safe_err_t e = SAFE_OK;

    if(want != NULL && *want != '\0') {
        b = find_backend(want);
        if(b == NULL) {
            printf("[WATCHDOG] 未知后端 %s，降级 null\n", want);
            b = &hal_watchdog_backend_none;
            e = b->init(timeout_ms);
        }
        else {
            e = b->init(timeout_ms);
            if(e != SAFE_OK && b != &hal_watchdog_backend_none) {
                printf("[WATCHDOG] 指定后端 %s 初始化失败(%d)，降级 null\n", b->name, e);
                b = &hal_watchdog_backend_none;
                e = b->init(timeout_ms);
            }
        }
    }
    else {
        b = pick_backend(timeout_ms);
        e = SAFE_OK;
    }

    s_backend      = b;
    s_companion    = NULL;
    s_started      = false;
    s_inited       = true;
    s_last_kick_ms = 0;

    /* 伴随层：主后端已用真狗时，再写一份心跳给 supervisor。
     * 只有显式给了心跳文件路径才启用——否则 PC 上会对着只读的 /var/run 刷失败日志。 */
    const char * hb = getenv("SAFE_WATCHDOG_FILE");
    if(s_backend != &hal_watchdog_backend_file && hb != NULL && *hb != '\0') {
        if(hal_watchdog_backend_file.init(timeout_ms) == SAFE_OK) {
            s_companion = &hal_watchdog_backend_file;
        }
        else {
            printf("[WATCHDOG] 软狗伴随层未启用（心跳文件不可用），只剩 %s\n", s_backend->name);
        }
    }

    printf("[WATCHDOG] 后端=%s timeout=%ums 节流=%ums 伴随层=%s\n",
           s_backend->name, timeout_ms, s_min_interval_ms,
           s_companion != NULL ? s_companion->name : "无");
    return e;
}

safe_err_t hal_watchdog_start(void)
{
    if(!s_inited) return SAFE_ERR_STATE;
    if(s_started) return SAFE_OK;
    safe_err_t e = s_backend->start();
    if(s_companion != NULL) s_companion->start();   /* 伴随层失败不影响主后端 */
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
    safe_err_t e = s_backend->kick();
    if(s_companion != NULL) s_companion->kick();
    return e;
}

safe_err_t hal_watchdog_stop(void)
{
    if(!s_inited) return SAFE_ERR_STATE;
    if(!s_started) return SAFE_OK;
    safe_err_t e = s_backend->stop();
    /* 伴随层先停：写 stopped 标记，supervisor 才不会把「正常退出」当成卡死。 */
    if(s_companion != NULL) s_companion->stop();
    if(e == SAFE_OK) s_started = false;
    return e;
}

void hal_watchdog_deinit(void)
{
    if(!s_inited) return;
    s_backend->deinit();
    if(s_companion != NULL) {
        s_companion->deinit();
        s_companion = NULL;
    }
    s_backend  = &hal_watchdog_backend_none;
    s_started  = false;
    s_inited   = false;
}

uint32_t hal_watchdog_kick_count(void)
{
    if(!s_inited || s_backend->kick_count == NULL) return 0;
    return s_backend->kick_count();
}
