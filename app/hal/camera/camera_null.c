/**
 * @file camera_null.c
 * 相机空实现：FM225 的 USB 视频流到货前占位。
 *
 * 所有采集类操作返回 SAFE_ERR_UNSUP，UI 侧据此把预览区显示为占位图案。
 */

#include "camera_backend.h"

#include <stddef.h>
#include <stdint.h>

static safe_err_t null_ok(void)
{
    return SAFE_OK;
}

static safe_err_t null_start(uint16_t w, uint16_t h)
{
    (void)w; (void)h;
    return SAFE_ERR_UNSUP;
}

static safe_err_t null_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    (void)rgb565; (void)info;
    return SAFE_ERR_UNSUP;
}

static int null_fd(void)
{
    return -1;   /* 无采集 fd：face 线程 poll 时跳过 */
}

const hal_camera_backend_t hal_camera_backend_null = {
    "null",
    null_ok,
    null_ok,
    null_start,
    null_ok,
    null_frame,
    NULL,
    null_fd,
};
