/**
 * @file hal_time.h
 * 时间基准抽象（需求 FR-6 / 阶段 2）。
 *
 * 阶段 1（Ubuntu）：直接返回系统时间 time(NULL)；
 * 阶段 5（i.MX6ULL）：替换为读 DS3231（I2C），断电不丢时间。
 * 业务层一律用 hal_time()，不 #include <time.h> 直接取时，方便将来切换。
 *
 * sync_time（MQTT 校时）在 Ubuntu 下用「偏移量」实现：记录 (目标 - 当前系统时间)，
 * 这样不要求 root 写系统时钟，也能让 TOTP 用统一基准跑通整条链路。
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TIME_SRC_SYS = 0, TIME_SRC_RTC } time_src_t;

/* 当前 Unix 时间戳（秒） */
uint32_t hal_time(void);

/* 校时：写入目标时间（RTC 路径下真正写硬件；系统时间路径下记录偏移量）。0=成功 */
int      hal_time_set(uint32_t t);

/* 时间源：阶段 1 返回 TIME_SRC_SYS */
time_src_t hal_time_source(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
