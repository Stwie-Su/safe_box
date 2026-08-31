/**
 * @file camera_service.c
 * 相机门面：当前绑空后端（FR-7 预留）。
 * UI 侧调用 hal_camera_frame() 拿不到帧时，预览区显示占位图案即可。
 */

#include "camera_backend.h"

#include <stdbool.h>

extern const hal_camera_backend_t hal_camera_backend_null;

static const hal_camera_backend_t * s_backend = &hal_camera_backend_null;
static bool s_started;

safe_err_t hal_camera_init(void)
{
    s_started = false;
    return s_backend->init();
}

safe_err_t hal_camera_deinit(void)
{
    if(s_started && s_backend->stop) s_backend->stop();
    s_started = false;
    return s_backend->deinit();
}

safe_err_t hal_camera_start(uint16_t w, uint16_t h)
{
    if(s_started) return SAFE_OK;
    safe_err_t e = s_backend->start(w, h);
    if(e == SAFE_OK) s_started = true;
    return e;
}

safe_err_t hal_camera_stop(void)
{
    if(!s_started) return SAFE_OK;
    safe_err_t e = s_backend->stop();
    if(e == SAFE_OK) s_started = false;
    return e;
}

safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    if(!s_started) return SAFE_ERR_STATE;
    return s_backend->frame(rgb565, info);
}

void hal_camera_release_frame(void)
{
    if(s_backend->release) s_backend->release();
}
