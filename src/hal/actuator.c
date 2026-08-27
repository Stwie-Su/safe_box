/**
 * @file actuator.c
 * 执行器实现：解锁后向某 IO 输出高电平（PC 端仅打印模拟，无真实 IO）。
 */
#include "actuator.h"
#include <stdio.h>

static bool g_state = false;

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
