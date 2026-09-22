/**
 * @file thread_util.h
 * 线程可观测性工具（服务于规约 §3.3 的「自创线程 ≤4」预算验收）。
 *
 * ★ 为什么需要这个头
 *   本项目自创 4 条线程：face（相机采集+串口）/ worker（异步作业）/ mqtt-net（远程通道）/
 *   actuator-off（执行器延时关断，临时）。但进程内还混着**不属于我们**的线程：
 *   PC 端实测 22 条，其中 8 条是 Mesa 的 llvmpipe（软件渲染）、约 9 条是 SDL2 内部线程
 *   （这两类板子端 FBDEV 构建都没有）。不命名的话 top -H / ps -L 里它们全显示进程名
 *   lvglsim，根本分不清谁是谁 —— 「自创线程 ≤4」这条预算也就**无从验收**。
 *   故统一在这里命名，并把平台差异集中在一处。
 *
 * ★ 三条平台坑（都封在这个头里，调用方不必关心）
 *   1. glibc 的 pthread_setname_np 受 __USE_GNU 保护，需要 _GNU_SOURCE，
 *      且**必须在 <pthread.h> 之前定义** → 本头必须是 .c 文件的**第一个 include**
 *      （放在任何系统头之前）。
 *   2. 名字长度上限 15 字符（Linux TASK_COMM_LEN=16，含结尾 '\0'），
 *      超了会被内核**静默拒绝**（不报错、也不生效）→ 这里统一截断。
 *   3. 非 glibc（musl/uclibc）可能没有该接口 → 退化成空实现，保证双平台都能构建。
 *
 * 用法（在线程入口函数开头自我命名）：
 *   #include "platform/thread_util.h"      // 必须是第一个 include
 *   static void * worker_main(void *p) { safe_thread_setname("safe-worker"); ... }
 */
#ifndef SAFE_PLATFORM_THREAD_UTIL_H
#define SAFE_PLATFORM_THREAD_UTIL_H

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <pthread.h>
#include <stddef.h>

/* 线程名长度上限：Linux TASK_COMM_LEN = 16，含结尾 '\0' → 实际可用 15。 */
#define SAFE_THREAD_NAME_MAX 15

#if defined(__GLIBC__)

/* 给**当前线程**命名。name 超过 15 字符会被截断（不截断内核会静默拒绝）。 */
static inline void safe_thread_setname(const char *name)
{
    char   buf[SAFE_THREAD_NAME_MAX + 1];
    size_t i = 0;

    if (!name) return;
    for (i = 0; i < (size_t)SAFE_THREAD_NAME_MAX && name[i] != '\0'; ++i) {
        buf[i] = name[i];
    }
    buf[i] = '\0';
    (void)pthread_setname_np(pthread_self(), buf);
}

#else  /* 非 glibc：没有该接口，退化为空实现，不阻断构建 */

static inline void safe_thread_setname(const char *name)
{
    (void)name;
}

#endif

#endif /* SAFE_PLATFORM_THREAD_UTIL_H */
