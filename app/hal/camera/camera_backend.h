/**
 * @file camera_backend.h
 * 相机后端接口（内部头文件，业务层不要包含）。
 */
#pragma once

#include "hal/hal_camera.h"

typedef struct {
    const char * name;
    safe_err_t (*init)(void);
    safe_err_t (*deinit)(void);
    safe_err_t (*start)(uint16_t w, uint16_t h);
    safe_err_t (*stop)(void);
    safe_err_t (*frame)(const uint8_t ** rgb565, hal_camera_frame_info_t * info);
    void       (*release)(void);
} hal_camera_backend_t;
