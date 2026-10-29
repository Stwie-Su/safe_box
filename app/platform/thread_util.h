/**
 * @file thread_util.h
 * 给自建线程起名字，方便用 ps -L / top -H 看清谁是谁。
 *
 * 为什么需要：进程运行起来后会同时存在好几条线程（我们自己的 + SDL2 / Mesa
 * 等第三方库的），不命名的话全显示成进程名，排查问题时分不清。
 *
 * 用法（每条线程的入口函数第一行）：
 *   #include "platform/thread_util.h"   ← 必须是第一个 include
 *   static void * worker_main(void * p) { safe_thread_setname("safe-worker"); ... }
 *
 * 两个注意点：
 *   1) 必须第一个 include：宏 _GNU_SOURCE 要在 <pthread.h> 之前定义；
 *   2) 名字最长 15 个字符，超了内核会静默忽略，所以这里主动截断。
 */
#ifndef SAFE_PLATFORM_THREAD_UTIL_H
#define SAFE_PLATFORM_THREAD_UTIL_H

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <pthread.h>
#include <stddef.h>

/* 线程名上限：Linux 的 TASK_COMM_LEN = 16，含结尾 '\0'，实际能用 15 个字符。 */
#define SAFE_THREAD_NAME_MAX 15

#if defined(__GLIBC__)

/* 给当前线程命名。超过 15 字符会被截断（不截断内核直接拒绝且不报错）。 */
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

#else  /* 非 glibc 没有这个接口：空实现，保证两个平台都能编过 */

static inline void safe_thread_setname(const char *name)
{
    (void)name;
}

#endif

#endif /* SAFE_PLATFORM_THREAD_UTIL_H */
