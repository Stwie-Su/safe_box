/**
 * @file face_backend.h
 * 人脸后端的函数表（内部头，业务层不要包含；对外用 hal_face.h）。
 *
 * 每个后端（假后端 / FM225 / 空后端）都要实现这张表里的函数，
 * 服务层拿到表之后统一调用，不知道底下具体是谁。
 *
 * 标了「可选」的成员可以是 NULL，服务层会当成「这个后端不支持」处理。
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

    /* 主线程每 20ms 调一次。now_ms 必须来自 hal_time_ms()，
     * 用别的时钟算超时会算出错误结果。 */
    void (*tick)(uint32_t now_ms);

    /* 录入（异步）：返回成功只表示「受理了」，结果通过事件上报。
     * user_name 会写进模组模板，方便以后核对；不支持写名的后端可以忽略。 */
    safe_err_t (*enroll)(const char * user_name);
    safe_err_t (*delete_tpl)(int32_t face_id);
    /* 清空模组里所有人脸。用于「本地和模组对不上」时重新拉齐。
     * 完成事件里 face_id 固定 -1。可为 NULL（不支持）。 */
    safe_err_t (*delete_all)(void);

    /* 触发一次识别，结果通过事件上报。忙返回 SAFE_ERR_BUSY。可为 NULL。 */
    safe_err_t (*verify_once)(void);

    /* 模组里的用户清单：>=0 条数，-1 没这个概念，-2 还没取到。可为 NULL。 */
    int32_t (*module_users)(int32_t * ids, int32_t cap);

    /* 模组健康状态。可为 NULL（当成「不知道」）。 */
    face_module_health_t (*module_health)(void);

    /* 模组实时状态：人脸在不在画面里、姿态、人脸框。都可为 NULL。 */
    int32_t (*face_state)(void);
    int32_t (*face_pose)(int16_t * yaw, int16_t * pitch, int16_t * roll);
    int32_t (*face_box)(int16_t * l, int16_t * t, int16_t * r, int16_t * b);

    /* 录入模式：是不是五向、已完成哪些方向、下一个录哪个、怎么切换。都可为 NULL。 */
    bool (*enroll_five_way)(void);
    uint8_t (*enroll_dir_mask)(void);
    uint8_t (*enroll_next_dir)(void);
    void (*set_enroll_five_way)(bool five_way);

    /* 注入假结果，只有假后端需要实现。 */
    safe_err_t (*inject)(int32_t face_id, face_reason_t reason);
} face_backend_t;

/* 各后端的实例。没编进构建的返回 NULL。 */
const face_backend_t * face_backend_fake(void);
const face_backend_t * face_backend_fm225(void);
const face_backend_t * face_backend_none(void);

/* 后端把结果交给服务层（只有后端实现会调）。
 * 内部会广播成 EV_FACE_EVENT 事件，主线程收到后再更新界面。 */
void hal_face_emit(face_event_t ev, const void * payload);

#ifdef __cplusplus
}
#endif
