/**
 * @file actuator.h
 * 执行器抽象层：负责「解锁后向某 IO 输出高电平」。
 * PC 端：actuator_drive() 仅打印状态，无真实 GPIO。
 */
#pragma once
#include <stdbool.h>

/* 初始化执行器（PC 端无操作） */
void actuator_init(void);

/* 驱动开锁机构：state=true 高电平（开锁），false 低电平（上锁） */
void actuator_drive(bool state);

/* 读取当前输出状态（供 UI 状态灯显示） */
bool actuator_get_state(void);
