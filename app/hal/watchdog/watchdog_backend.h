/**
 * @file watchdog_backend.h
 * 看门狗后端接口（**内部头文件，业务层 / UI 层禁止包含**；请用 hal/hal_watchdog.h）。
 *
 * 【这是什么】一张函数指针表：把「一个看门狗后端要具备哪些能力」声明成
 * 数据类型。与 camera / time / face 等模块同构——上层只认接口不认实现。
 *
 * 【谁用它】只有 watchdog_service.c：它持有 `const hal_watchdog_backend_t * s_backend`，
 * 把 hal_watchdog_xxx() 转发成 s_backend->xxx()。
 *
 * 【实例在哪】
 *   - hal_watchdog_backend_linux 定义在 backend_linux.c 文件末尾
 *   - hal_watchdog_backend_none  定义在 backend_none.c  文件末尾
 *   二者都是 const 全局对象，编译期定好，运行期只读。
 *
 * ★ 成员顺序必须与本结构体定义严格一致：C 的位置初始化若错位不会报错，但会调错函数。
 */
#pragma once

#include "hal/hal_watchdog.h"

typedef struct {
    const char * name;                      /* 后端名，用于 SAFE_WATCHDOG_BACKEND 匹配 */
    safe_err_t (*init)(uint32_t timeout_ms);/* 打开设备并设定超时（不开始喂狗） */
    safe_err_t (*start)(void);              /* 开始喂狗周期（立即喂第一口） */
    safe_err_t (*kick)(void);               /* 喂狗一次 */
    safe_err_t (*stop)(void);               /* 停止计时（magic close） */
    void       (*deinit)(void);             /* 关闭设备 */
    uint32_t   (*kick_count)(void);         /* 实际喂狗次数（可观测） */
} hal_watchdog_backend_t;
