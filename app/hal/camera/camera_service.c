/**
 * @file camera_service.c
 * 相机门面：绑定后端、转发帧请求（FR-16 视频预览）。
 *
 * 后端选择：环境变量 SAFE_CAMERA_BACKEND（v4l2 / null），默认 v4l2；
 * v4l2 初始化失败（无摄像头/无权限）时自动降级 null，不让系统起不来——
 * 与 face_service 的降级策略一致。
 * UI 侧调用 hal_camera_frame() 拿不到帧时，预览区显示占位图案即可。
 */

#include "camera_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const hal_camera_backend_t hal_camera_backend_null;
extern const hal_camera_backend_t hal_camera_backend_v4l2;

static const hal_camera_backend_t * s_backend = &hal_camera_backend_null;
static bool s_started;
static bool s_inited;

/* 后端注册表：name 匹配；name==NULL 时首个非空项（表序即优先级） */
static const hal_camera_backend_t * find_backend(const char * name)
{
    const hal_camera_backend_t * table[] = {
        &hal_camera_backend_v4l2,
        &hal_camera_backend_null,
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i] != NULL && (name == NULL || strcmp(name, table[i]->name) == 0))
            return table[i];
    }
    return NULL;
}

safe_err_t hal_camera_init(void)
{
    if (s_inited) return SAFE_OK;

    const char * want = getenv("SAFE_CAMERA_BACKEND");
    const hal_camera_backend_t * b = find_backend((want && *want) ? want : "v4l2");
    if (b == NULL) b = &hal_camera_backend_null;   /* 未知名字 → 降级 */

    safe_err_t e = b->init();
    if (e != SAFE_OK && b != &hal_camera_backend_null) {
        printf("[CAMERA] 后端 %s 初始化失败(%d)，降级 null 预览占位\n", b->name, e);
        b = &hal_camera_backend_null;
        e = b->init();
    }

    s_backend = b;
    s_started = false;
    s_inited  = true;
    printf("[CAMERA] 后端=%s\n", s_backend->name);
    return e;
}

safe_err_t hal_camera_deinit(void)
{
    if (!s_inited) return SAFE_OK;
    if (s_started && s_backend->stop) s_backend->stop();
    s_backend->deinit();
    s_backend = &hal_camera_backend_null;
    s_started = false;
    s_inited  = false;
    return SAFE_OK;
}

safe_err_t hal_camera_start(uint16_t w, uint16_t h)
{
    if (!s_inited) return SAFE_ERR_STATE;
    if (s_started) return SAFE_OK;
    safe_err_t e = s_backend->start(w, h);
    if (e == SAFE_OK) s_started = true;
    return e;
}

safe_err_t hal_camera_stop(void)
{
    if (!s_inited) return SAFE_ERR_STATE;
    if (!s_started) return SAFE_OK;
    safe_err_t e = s_backend->stop();
    if (e == SAFE_OK) s_started = false;
    return e;
}

safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    if (!s_inited || !s_started) return SAFE_ERR_STATE;
    return s_backend->frame(rgb565, info);
}

void hal_camera_release_frame(void)
{
    if (s_backend->release) s_backend->release();
}

