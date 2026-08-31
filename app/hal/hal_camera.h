/**
 * @file hal_camera.h
 * 相机抽象（FR-7 视频预览预留）。
 *
 * 当前为空实现（FM225 的 USB 视频流未接入）。
 * 阶段 4 接入 V4L2 MJPEG 后新增 camera_v4l2.c 后端，本头文件不变。
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t  width;
    uint16_t  height;
    uint32_t  seq;          /* 帧序号 */
    uint32_t  timestamp;    /* hal_time() */
} hal_camera_frame_info_t;

safe_err_t hal_camera_init(void);
safe_err_t hal_camera_deinit(void);

/* 开始采集。w/h 为期望分辨率，后端可协商（需求锁定 320×240）。 */
safe_err_t hal_camera_start(uint16_t w, uint16_t h);
safe_err_t hal_camera_stop(void);

/* 取一帧 RGB565；无新帧返回 SAFE_ERR_BUSY。缓冲区归后端所有，
 * 用完必须调 hal_camera_release_frame()，否则后端可能停更。 */
safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info);
void       hal_camera_release_frame(void);

#ifdef __cplusplus
}
#endif
