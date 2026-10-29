/**
 * @file perf_probe.h
 * 性能探针：设 SAFE_PERF_LOG=1 时，每 2 秒打一行主循环耗时。
 *
 * 为什么不用 LVGL 自带的性能面板：它会在右下角画一层 FPS 数字，本身要渲染、
 * 还会挡住界面；而且只给 FPS，看不出偶尔卡一下（尖峰）。这里直接统计
 * lv_timer_handler() 每次花多久：avg 看平均开销，max 看有没有卡顿。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 读环境变量决定开不开，开了就顺手挂一个定时器。开机调一次。 */
void perf_probe_init_from_env(void);

/* 探针是否已开启（关掉时代码可以少做几次取时间调用）。 */
bool perf_probe_enabled(void);

/* 主循环里每转一圈记一次：传这一圈 lv_timer_handler() 的耗时（毫秒）。 */
void perf_probe_record(uint32_t cost_ms);
