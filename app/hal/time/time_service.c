/**
 * @file time_service.c
 * 时间源门面：按 SAFE_TIME_BACKEND 在运行期选择后端（sys / rtc），上层无感知。
 *
 * 选择结果只在启动时算一次（time_service_init，幂等）；未被编排层调用的路径
 * （单测等）在首次取时时懒选择一次，默认 sys——与加后端之前的行为完全一致。
 *
 * DS3231 到货前板上没有 /dev/rtc1：即使设了 SAFE_TIME_BACKEND=rtc 也会探测失败
 * 并自动降级 sys（打一条日志，不崩不卡），这正是到货前的常态路径。
 */

#include "time_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const hal_time_backend_t hal_time_backend_sys;
extern const hal_time_backend_t hal_time_backend_rtc;

static const hal_time_backend_t * s_backend = &hal_time_backend_sys;
static bool                       s_inited;

/* 按 SAFE_TIME_BACKEND 选后端：rtc 只有在节点存在且 RTC_RD_TIME 成功时才生效。 */
static void time_service_select(void)
{
    const char * want = getenv("SAFE_TIME_BACKEND");
    if(want == NULL || *want == '\0') want = "sys";

    if(strcmp(want, "rtc") == 0) {
        const char * dev = time_rtc_dev();
        if(time_rtc_available()) {
            s_backend = &hal_time_backend_rtc;
            printf("[time] 后端 = rtc（%s，外部 RTC，断电保持）\n", dev);
            return;
        }
        printf("[time] 请求 rtc 后端，但 %s 不可用（节点不存在或 RTC_RD_TIME 失败），降级 sys\n",
               dev);
    }
    else if(strcmp(want, "sys") != 0) {
        printf("[time] 未知 SAFE_TIME_BACKEND=%s，按 sys 处理\n", want);
    }

    s_backend = &hal_time_backend_sys;
    printf("[time] 后端 = sys（系统时间，断电丢失）\n");
}

void time_service_init(void)
{
    if(s_inited) return;
    time_service_select();
    s_inited = true;
}

/* 懒选择兜底：未调 init 的路径首次取时按默认（sys）选一次。 */
static void time_service_ensure(void)
{
    if(!s_inited) time_service_init();
}

uint32_t hal_time(void)
{
    time_service_ensure();
    return s_backend->now();
}

uint32_t hal_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

safe_err_t hal_time_set(uint32_t unix_seconds)
{
    time_service_ensure();
    return s_backend->set(unix_seconds);
}

time_src_t hal_time_source(void)
{
    time_service_ensure();
    return s_backend->src;
}
