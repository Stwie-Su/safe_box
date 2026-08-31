/**
 * @file perf_probe.h
 * 性能探针：环境变量 SAFE_PERF_LOG=1 开启，每 2 秒打一行主循环耗时。
 *
 * 为什么不用 LV_USE_PERF_MONITOR：它会在右下角画 FPS 覆盖层，本身要渲染、
 * 还污染画面，不适合量产；而且只给 FPS，看不出抖动。这里直接统计
 * lv_timer_handler() 的单次耗时，avg 看稳态开销，max 看卡顿尖峰。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 读环境变量决定是否开启，开启则挂定时器。 */
void perf_probe_init_from_env(void);

bool perf_probe_enabled(void);

/* 主循环里记录一次 lv_timer_handler 的耗时。 */
void perf_probe_record(uint32_t cost_ms);
