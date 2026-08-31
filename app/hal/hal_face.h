/**
 * @file hal_face.h
 * 人脸识别模块接口（业务层唯一可见的人脸头文件）。
 *
 * 设计约束：
 *  1. 本头文件不出现任何平台头文件，业务层包含它不会被污染；
 *  2. 异步事件驱动，业务层不轮询硬件；
 *  3. 后端可替换：模拟后端与 FM225 后端实现同一张 vtable，
 *     由构建选项 SAFE_FACE_BACKEND 决定编哪个，业务代码零改动；
 *  4. 业务层只认 {face_id, score} 这一个数据契约，不关心数据从哪来。
 *
 * FM225 到货后只需补 app/hal/face/backend_fm225.c 里的 UART 解析，
 * 本文件与所有调用方都不用改。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 数据契约 ---------------- */

typedef struct {
    int32_t  face_id;      /* -1 = 未识别到已注册人脸 */
    int32_t  score;        /* 0~100，越界值业务层视为无效 */
    uint32_t seq;          /* 帧序号，用于去重与调试 */
    uint32_t timestamp;    /* hal_time() 时间戳 */
} face_result_t;

typedef struct {
    int32_t    face_id;    /* 成功 >= 0，失败 -1 */
    safe_err_t err;
} face_enroll_result_t;

typedef struct {
    int32_t    face_id;
    safe_err_t err;
} face_delete_result_t;

/* ---------------- 事件 ---------------- */

typedef enum {
    FACE_EV_DETECT = 0,   /* 识别结果：payload = const face_result_t * */
    FACE_EV_ENROLL_DONE,  /* 录入完成：payload = const face_enroll_result_t * */
    FACE_EV_DELETE_DONE,  /* 删除完成：payload = const face_delete_result_t * */
    FACE_EV_ERROR,        /* 后端异常：payload = const char * */
} face_event_t;

typedef void (*face_event_cb_t)(face_event_t ev, const void * payload, void * user);

/* ---------------- 能力 ---------------- */

#define FACE_CAP_DETECT    (1u << 0)   /* 能产出识别结果 */
#define FACE_CAP_ENROLL    (1u << 1)   /* 支持录入 */
#define FACE_CAP_DELETE    (1u << 2)   /* 支持删除 */
#define FACE_CAP_INJECT    (1u << 3)   /* 支持注入模拟结果（仅 fake 后端） */
#define FACE_CAP_LIVENESS  (1u << 4)   /* 带活体检测 */

typedef struct {
    const char * name;             /* "fake" / "fm225" / "none" */
    uint32_t     caps;
    int32_t      max_templates;    /* 模组可容纳的模板数，-1 = 不限 */
} face_caps_t;

/* ---------------- 生命周期 ---------------- */

/* 初始化并绑定后端。backend_name 为 NULL 时用构建期默认后端。 */
safe_err_t face_service_init(const char * backend_name);
safe_err_t face_service_deinit(void);

/* 主线程周期驱动（20ms 级）。后端需要轮询硬件时在此处理。 */
void face_service_tick(uint32_t now_ms);

/* ---------------- 订阅 ---------------- */

safe_err_t face_service_subscribe(face_event_cb_t cb, void * user);
void       face_service_unsubscribe(face_event_cb_t cb, void * user);

/* ---------------- 查询与控制 ---------------- */

const face_caps_t * face_service_caps(void);

safe_err_t face_service_start(void);
safe_err_t face_service_stop(void);
bool       face_service_running(void);

/* 异步录入：立即返回，结果通过 FACE_EV_ENROLL_DONE 上报。 */
safe_err_t face_service_enroll_async(void);

/* 异步删除指定模板。 */
safe_err_t face_service_delete_async(int32_t face_id);

/* 最近一次识别结果；没有任何结果时返回 NULL。 */
const face_result_t * face_service_last_result(void);

/* ---------------- 调试注入 ---------------- */

/* 仅 FACE_CAP_INJECT 后端有效（当前为 fake），其它后端返回 SAFE_ERR_UNSUP。
 * 供 UI 滑块与 RPC inject_score 使用，真实硬件路径不会走到这里。 */
safe_err_t face_service_inject(int32_t face_id, int32_t score);

/* 兼容层：旧同步轮询接口，内部取最近一次结果。
 * 迁移期保留，待 auth_fsm 完全改为事件驱动后删除。 */
int face_service_poll_compat(face_result_t * out);

#ifdef __cplusplus
}
#endif
