/**
 * @file hal_face.h
 * 人脸识别模块接口（业务层唯一可见的人脸头文件）。
 *
 * 设计约束：
 *  1. 本头文件不出现任何平台头文件，业务层包含它不会被污染；
 *  2. 异步事件驱动，业务层不轮询硬件；
 *  3. 后端可替换：模拟后端与 FM225 后端实现同一张 vtable，
 *     由构建选项 SAFE_FACE_BACKEND 决定编哪个，业务代码零改动；
 *  4. 业务层只认 {face_id, reason} 这一个数据契约，不关心数据从哪来。
 *     FM225 模组不输出置信度分数，识别结果只按失败原因分流（需求 v1.6 FR-19）。
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

/* 识别结果按失败原因分流（FM225 无分数；auth_fsm 依赖本类型） */
typedef enum {
    FACE_RES_OK = 0,        /* 匹配成功 */
    FACE_RES_NO_MATCH,      /* 未匹配：计入失败，连续达阈值转动态码 */
    FACE_RES_LIVENESS_FAIL, /* 活体失败：拒绝 + 防伪告警 */
    FACE_RES_TIMEOUT,       /* 超时 / 无人脸：不计数 */
    FACE_RES_ERROR,         /* 模组/链路异常：不计数 */
} face_reason_t;

typedef struct {
    int32_t       face_id;  /* -1 = 未识别到已注册人脸 */
    face_reason_t reason;   /* 结果原因码（取代旧 score 字段） */
    uint32_t      seq;      /* 帧序号，用于去重与调试 */
    uint32_t      timestamp;/* hal_time() 时间戳（Unix 秒，不是进程起算秒，见 D12） */
    /* 模组返回的用户名（v1.4 新增，FR-21 防线 1 的数据源）。
     * FM225 的 VERIFY 成功应答按手册 P34 s_msg_reply_verify_data 携带 user_name[32]；
     * 业务层用它核对「这张脸是不是它声称的那个人」。空串 = 模组未提供该名字
     * （历史模板未写名 / 非 FM225 后端）——调用方必须跳过核对，不得据此判凭据失效。 */
    char          user_name[32];
} face_result_t;

/* 原因码的可读名（日志 / RPC status 用），非法值返回 "unknown"。
 * 实现在 face_service.c。 */
const char * face_reason_name(face_reason_t r);

typedef struct {
    int32_t    face_id;    /* 成功 >= 0，失败 -1 */
    safe_err_t err;
    /* 已完成的方向掩码（手册 V1.7 §MID_ENROLL REPLY 的 s_msg_reply_enroll_data）：
     * 低 5 位从高到低 = 上(0x10) / 下(0x08) / 左(0x04) / 右(0x02) / 正(0x01)，
     * 位为 1 表示该朝向已录入。0x1F = 五向全部完成。
     * 单帧模式或未取到时填 0；调用方据此显示进度芯片。 */
    uint8_t    face_dir_mask;
} face_enroll_result_t;

typedef struct {
    int32_t    face_id;
    safe_err_t err;
} face_delete_result_t;

/* ---------------- 事件 ---------------- */

/* 人脸事件不再走本模块的自有订阅接口（v1.3 §5.18：全工程只留 worker + event_bus
 * 两套通知机制）。后端结果经 face_service_emit() → event_bus(EV_FACE_EVENT) 广播，
 * payload 为 ev_face_event_t（core/event_bus.h，POD 按值拷贝）。
 * FACE_EV_ERROR 的描述文本放在该结构的 msg[64] 定长数组里。 */
typedef enum {
    FACE_EV_DETECT = 0,   /* 识别结果：ev_face_event_t.res */
    FACE_EV_ENROLL_DONE,  /* 录入完成：ev_face_event_t.enroll */
    FACE_EV_DELETE_DONE,  /* 删除完成：ev_face_event_t.del */
    FACE_EV_ERROR,        /* 后端异常：ev_face_event_t.msg */
} face_event_t;

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

/* 主线程周期驱动（20ms 级）。后端需要轮询硬件时在此处理。
 * now_ms 必须与 hal_time_ms() 同源（单调毫秒）——见 face_backend_t.tick 契约。 */
void face_service_tick(uint32_t now_ms);

/* ---------------- 查询与控制 ---------------- */

const face_caps_t * face_service_caps(void);

safe_err_t face_service_start(void);
safe_err_t face_service_stop(void);
bool       face_service_running(void);

/* Duty-cycle（FR-27 模组功耗管理）：人脸页是否前台。离开前台 = 停 UVC 采集流
 * + 终止/不再发起模组识别会话（红外补光与传感停工作降温）；回到前台恢复。
 * 由 page_face 在可见性边沿调用；face 线程 / fm225 后端经 face_service_foreground()
 * 读取。跨线程语义与 face_thread 的退出标志相同：volatile 标量、无锁。 */
void face_service_set_foreground(bool active);
bool face_service_foreground(void);

/* 异步录入：立即返回，结果通过 FACE_EV_ENROLL_DONE 上报。
 * user_name（v1.4）：本地用户名，由后端写入模组侧模板，使 VERIFY 应答能把它带回来
 * 供 FR-21 凭据一致性核对（backend → face_result_t.user_name → auth_fsm）。
 * 传 NULL 或空串 = 不写名（核对会自动跳过）。 */
safe_err_t face_service_enroll_async(const char * user_name);

/* 异步删除指定模板。 */
safe_err_t face_service_delete_async(int32_t face_id);

/* 单次识别（FR-27 拍板 2026-09-15）：触发一次识别会话，结果/超时走既有事件链，
 * 会话结束即静默（模组红外与传感停工作）。识别不再自动循环；忙 = SAFE_ERR_BUSY，
 * 后端不支持 = SAFE_ERR_UNSUP。 */
safe_err_t face_service_verify_once(void);

/* 模组实时人脸状态（FR-19 实时引导数据源，NOTE NID_FACE_STATE 的 state 字段）。
 * 语义（手册 §NOTE）：0=人脸正常 1=未检测到人脸 2=太靠上 3=太靠下 4=太靠左 5=太靠右；
 * -1 = 尚未收到任何状态帧（该后端无此概念或模组未上报）。
 * 后端在 face 线程持续更新，此处只读快照；无模组概念的后端恒返回 -1。 */
int32_t face_service_face_state(void);

/* 人脸姿态快照（五向录入引导用）：yaw/pitch/roll，任一可为 NULL。
 * 返回 0 = 取到；< 0 = 后端无姿态概念或未上报。
 * 与 face_service_face_state() 属于**同一帧的两个侧面**，调用方应成对读取。 */
int32_t face_service_face_pose(int16_t * yaw, int16_t * pitch, int16_t * roll);

/* 当前录入模式是否为五向（face_direction=0x1F）。false = 单帧。
 * 由后端 env SAFE_FACE_ENROLL_5WAY 决定，供录入页切换引导文案。 */
bool face_service_enroll_five_way(void);

/* 是否有录入会话在途（录入实时引导小字的显隐依据）。 */
bool face_service_enrolling(void);

/* 模组侧已注册用户清单（FR-21 防线 3「启动对账」用）。
 * 返回：>= 0 模组清单数量（**可以是 0**，0 与「没取到」必须区分开）；
 *       -1    该后端没有「模组清单」概念（fake / none）；
 *       -2    后端有该能力但清单尚未取得（模组未就绪 / 尚未应答）。
 * cap <= 0 时只查询数量（ids 可传 NULL）。
 * 上层用它核对「本地凭据 ↔ 模组存量」，孤儿凭据（本地有、模组无）应标失效且不用于放行。 */
int32_t face_service_module_users(int32_t * ids, int32_t cap);

/* ---------------- 模组健康（FR-23） ---------------- */

/* 模组健康三态（FR-23「模组健康与降级」）。与 face_service_module_users 的三态风格一致，
 * 业务层（UI / RPC）只读，不得直接读后端全局变量：
 *   FACE_MOD_UNKNOWN = 未知：后端无模组级监测（fake / none），或尚未得出判定；
 *   FACE_MOD_OK      = 健康：近期收到过模组合法帧（NOTE READY / VERIFY 应答 / 清单应答）；
 *   FACE_MOD_FAIL    = 不健康：连续多轮无任何应答（约 15s），判定为模组无响应。
 * 不健康时上层应界面告警并自动降级为 PIN / 动态码认证（本接口只读，不改变模组状态）。 */
typedef enum {
    FACE_MOD_UNKNOWN = 0,
    FACE_MOD_OK      = 1,
    FACE_MOD_FAIL    = 2,
} face_module_health_t;

/* 只读查询当前模组健康（FR-23）。返回上面三态之一。 */
face_module_health_t face_service_module_health(void);

/* 健康三态的可读名（日志 / RPC status 用）："unknown" / "ok" / "fail"。
 * 实现在 face_service.c。非法值返回 "unknown"。 */
const char * face_module_health_name(face_module_health_t h);

/* 最近一次识别结果；没有任何结果时返回 NULL。 */
const face_result_t * face_service_last_result(void);

/* ---------------- 调试注入 ---------------- */

/* 仅 FACE_CAP_INJECT 后端有效（当前为 fake），其它后端返回 SAFE_ERR_UNSUP。
 * 供调试页与 RPC inject_face 使用，真实硬件路径不会走到这里。 */
safe_err_t face_service_inject(int32_t face_id, face_reason_t reason);

/* 兼容层：旧同步轮询接口，内部取最近一次结果。
 * 迁移期保留，待 auth_fsm 完全改为事件驱动后删除。 */
int face_service_poll_compat(face_result_t * out);

#ifdef __cplusplus
}
#endif

