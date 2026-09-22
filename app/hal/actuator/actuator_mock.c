/**
 * @file actuator_mock.c
 * 执行器模拟后端（PC 阶段 / LED 验证阶段）。
 *
 * 不碰硬件，只打印动作日志；关断动作放在独立线程里完成，
 * 与真实 GPIO 后端的时序保持一致（调用方非阻塞返回，到点自动拉低）。
 *
 * 阶段 3 换成 actuator_gpio.c 时，把 pulse()/release() 换成写 sysfs 或字符设备，
 * 本文件与业务层都不受影响。
 */

#include "platform/thread_util.h"      /* ★ 必须第一个 include：它要在 <pthread.h> 前定义 _GNU_SOURCE */

#include "actuator_backend.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_t     s_off_thread;
static volatile bool s_thread_busy;

static void * off_thread_fn(void * arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    safe_thread_setname("safe-act-off");
    usleep(ms * 1000u);
    printf("[ACTUATOR] pulse end after %u ms -> LOW\n", (unsigned)ms);
    hal_actuator_notify_low();
    s_thread_busy = false;
    return NULL;
}

static safe_err_t mock_init(void)
{
    s_thread_busy = false;
    printf("[ACTUATOR] mock backend ready (no hardware)\n");
    return SAFE_OK;
}

static safe_err_t mock_pulse(uint32_t ms)
{
    if(s_thread_busy) return SAFE_ERR_BUSY;

    printf("[ACTUATOR] pulse start -> HIGH for %u ms\n", (unsigned)ms);
    s_thread_busy = true;
    if(pthread_create(&s_off_thread, NULL, off_thread_fn, (void *)(uintptr_t)ms) != 0) {
        s_thread_busy = false;
        return SAFE_ERR_FAIL;
    }
    pthread_detach(s_off_thread);
    return SAFE_OK;
}

static safe_err_t mock_release(void)
{
    printf("[ACTUATOR] force LOW\n");
    hal_actuator_notify_low();
    return SAFE_OK;
}

const hal_actuator_backend_t hal_actuator_backend_mock = {
    "mock",
    mock_init,
    mock_pulse,
    mock_release,
};
