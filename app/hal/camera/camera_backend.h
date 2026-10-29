/**
 * @file camera_backend.h
 * 相机后端的函数表（内部头，业务层用 hal_camera.h，不要包含这个）。
 *
 * 真摄像头（backend_v4l2.c）和「没有相机」（backend_none.c）都实现同一张表，
 * 服务层拿着表统一调用，这就是 C 语言里「面向接口编程」的写法。
 * 单元测试也可以塞一个假后端，不用真硬件。
 *
 * 每个字段的含义和 hal_camera.h 里同名函数一样，那里写得更详细。
 */
#pragma once

#include "hal/hal_camera.h"

typedef struct {
    const char * name;                  /* 后端名，用 SAFE_CAMERA_BACKEND 选 */
    safe_err_t (*init)(void);           /* 打开设备（不开流） */
    safe_err_t (*deinit)(void);         /* 关设备、释放缓冲 */
    safe_err_t (*start)(uint16_t w, uint16_t h);   /* 协商格式、申请缓冲、开流 */
    safe_err_t (*stop)(void);           /* 停流（保留缓冲，方便快速恢复） */
    safe_err_t (*frame)(const uint8_t ** rgb565, hal_camera_frame_info_t * info);
                                        /* 取最新一帧，缓冲归后端所有，要 release */
    void       (*release)(void);        /* 归还帧 */
    int        (*fd)(void);             /* 采集 fd，给 face 线程 poll；没有返回 -1 */
} hal_camera_backend_t;
