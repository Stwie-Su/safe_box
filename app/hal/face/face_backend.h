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

    /* 主线程周期驱动。now_ms 必须与 hal_time_ms() 同源（单调毫秒）——后端内部
     * 一律用 hal_time_ms() 记时刻；跨时钟基会让「now_ms - 记录时刻」无符号回绕、
     * 超时判定恒真（D1）。这是契约，不许改。 */
    void (*tick)(uint32_t now_ms);

    /* 异步录入：返回 SAFE_OK 表示已受理，结果由 face_service_emit(FACE_EV_ENROLL_DONE) 上报。
     * user_name（v1.4）：本地用户名，后端应写入模组侧模板名（FM225 的 ENROLL 载荷
     * 带 user_name[32] 字段），使 VERIFY 应答能带回该名供 FR-21 核对。后端可以忽略
     * 该参数（不支持写名的后端），此时模组返回用户名为空、核对自动跳过。 */
    safe_err_t (*enroll)(const char * user_name);
    safe_err_t (*delete_tpl)(int32_t face_id);

    /* 单次识别（FR-27 拍板：识别由界面按钮触发，不再自动循环）。受理后结果经
     * face_service_emit(FACE_EV_DETECT) 上报，会话结束即静默（低功耗）。
     * 忙返回 SAFE_ERR_BUSY；可为 NULL（视为不支持，SAFE_ERR_UNSUP）。 */
    safe_err_t (*verify_once)(void);

    /* 模组侧已注册用户清单（FR-21 防线 3「启动对账」用）。语义同 hal_face.h 的
     * face_service_module_users：>=0 = 清单数量；-1 = 该后端无此概念；-2 = 尚未取得。
     * 可为 NULL（视为 -1）。 */
    int32_t (*module_users)(int32_t * ids, int32_t cap);

    /* 模组健康只读三态（FR-23）。语义见 hal_face.h 的 face_module_health_t。
     * 可为 NULL —— 视为 FACE_MOD_UNKNOWN（该后端没有模组级健康监测，如 fake / none）。 */
    face_module_health_t (*module_health)(void);

    /* 模组实时人脸状态（FR-19 引导，NOTE NID_FACE_STATE 的 state 字段）。
     * 语义见 hal_face.h 的 face_service_face_state()。
     * 可为 NULL（视为 -1 未知，如 fake / none）。 */
    int32_t (*face_state)(void);
    /* 姿态快照（五向录入引导的数据源）：yaw/pitch/roll，任一输出参数可为 NULL。
     * 返回 0 = 取到；< 0 = 该后端无姿态概念。**可选接口**，可为 NULL。
     * 语义：NOTE NID_FACE_STATE 的 8 个 int16 中的第 6/7/8 个（小端，实测）。 */
    int32_t (*face_pose)(int16_t * yaw, int16_t * pitch, int16_t * roll);
    /* 当前录入模式是否为五向。**可选接口**，NULL 视为单帧。 */
    bool (*enroll_five_way)(void);

    /* 注入模拟结果，仅 FACE_CAP_INJECT 后端需要实现 */
    safe_err_t (*inject)(int32_t face_id, face_reason_t reason);
} face_backend_t;

/* 各后端工厂：未编入构建时返回 NULL */
const face_backend_t * face_backend_fm225(void);
const face_backend_t * face_backend_none(void);

/* 后端向服务层上报结果（只有后端实现可以调用）。
 * 事件经 event_bus(EV_FACE_EVENT) 广播（步骤 3a）；FACE_EV_ERROR 的
 * const char* 描述由 emit 拷进 ev_face_event_t.msg[64] 定长数组，无跨线程悬空。 */
void face_service_emit(face_event_t ev, const void * payload);

#ifdef __cplusplus
}
#endif

