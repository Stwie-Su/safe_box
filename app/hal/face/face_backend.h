/**
 * @file face_backend.h
 * 人脸后端接口（内部头文件，业务层不要包含）。
 *
 * 每个后端实现同一张 vtable，由构建选项 SAFE_FACE_BACKEND 决定编哪一个。
 * 后端完成异步作业后，调用 face_service_emit() 把结果抛回服务层。
 */
#pragma once

#include "hal/hal_face.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct face_backend {
    const char * name;
    uint32_t     caps;
    int32_t      max_templates;

    safe_err_t (*init)(void);
    safe_err_t (*deinit)(void);
    safe_err_t (*start)(void);
    safe_err_t (*stop)(void);

    /* 主线程周期驱动，now_ms 为毫秒计时 */
    void (*tick)(uint32_t now_ms);

    /* 异步录入：返回 SAFE_OK 表示已受理，结果由 face_service_emit(FACE_EV_ENROLL_DONE) 上报 */
    safe_err_t (*enroll)(void);
    safe_err_t (*delete_tpl)(int32_t face_id);

    /* 注入模拟结果，仅 FACE_CAP_INJECT 后端需要实现 */
    safe_err_t (*inject)(int32_t face_id, int32_t score);
} face_backend_t;

/* 各后端工厂：未编入构建时返回 NULL */
const face_backend_t * face_backend_fake(void);
const face_backend_t * face_backend_fm225(void);
const face_backend_t * face_backend_none(void);

/* 后端向服务层上报结果（只有后端实现可以调用） */
void face_service_emit(face_event_t ev, const void * payload);

#ifdef __cplusplus
}
#endif
