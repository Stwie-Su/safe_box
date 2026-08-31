/**
 * @file actuator.c
 * 执行器实现：解锁后向某 IO 输出高电平（PC 端仅打印模拟，无真实 IO）。
 *
 * 阶段 1 扩展：新增 actuator_pulse()。
 * 需求 ADR-6 / NFR-7：继电器动作必须有【硬性时长上限 500ms】，超时强制关闭，
 * 防止软件卡死把锁一直开着烧线圈。PC 阶段用打印 + 后台线程模拟「高脉冲 500ms 后自动回低」。
 *
 * 真实硬件（阶段 3）：actuator_pulse 内部改为
 *   GPIO 拉高 → 后台线程 usleep(钳位后时长) → GPIO 拉低。
 */
#include "actuator.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

static bool g_state = false;

/* 后台脉冲线程：置高 -> 睡眠 duration -> 置低。独立线程避免阻塞 UI 主循环。 */
static void * pulse_thread(void * arg)
{
    long ms = (long)arg;
    actuator_drive(true);
    usleep((useconds_t)ms * 1000);
    actuator_drive(false);
    return NULL;
}

void actuator_init(void)
{
    printf("[ACTUATOR] PC mock mode: no real GPIO. init OK (state=LOW)\n");
    g_state = false;
}

void actuator_drive(bool state)
{
    g_state = state;
    printf("[ACTUATOR] OUTPUT %s (lock %s)\n",
           state ? "HIGH" : "LOW", state ? "UNLOCKED" : "LOCKED");
}

bool actuator_get_state(void)
{
    return g_state;
}

void actuator_pulse(uint32_t duration_ms)
{
    /* ★ 硬性上限保护：任何调用方传入的时长一律钳位到 500ms（NFR-7） */
    if (duration_ms > 500) duration_ms = 500;
    if (duration_ms == 0)  duration_ms = 500;

    pthread_t t;
    if (pthread_create(&t, NULL, pulse_thread, (void *)(long)duration_ms) != 0) {
        /* 创建线程失败（极端情况）退化为同步：直接置高再延时回低 */
        actuator_drive(true);
        usleep(duration_ms * 1000);
        actuator_drive(false);
        return;
    }
    pthread_detach(t);   /* 用完即弃，无需 join */
}
