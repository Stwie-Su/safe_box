/**
 * @file auth_fsm.h
 * 置信度分级状态机（需求 FR-2 / 核心）。
 *
 * 这是人脸开锁链路的「大脑」：接收 hal_face 的识别结果（阶段 1 来自 UI/RPC 注入），
 * 按置信度分三档 → UNLOCK / WAIT_OTP / DENY，并负责失败计数与设备级锁定。
 *
 * 与 UI 解耦：状态变化通过 auth_ui_hook 回调通知 UI 层（切页、弹窗、提示），
 * 不直接 #include LVGL（NFR-5：业务层不依赖 UI / 平台头文件）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSM_IDLE = 0,     /* 待机 */
    FSM_DETECTING,    /* 识别中（真实 FM225 轮询期间） */
    FSM_UNLOCKED,     /* 已开锁（展示 3s） */
    FSM_WAIT_OTP,     /* 等待动态码（弹动态密码页，60s 超时） */
    FSM_DENY,         /* 拒绝（展示 2s） */
    FSM_LOCKOUT,      /* 设备级锁定（倒计时 30s） */
} fsm_state_t;

/* UI 通知回调：状态机进入某状态时调用（UI 据此切页/弹窗/提示） */
typedef void (*auth_ui_hook_t)(fsm_state_t st, const char *user, const char *detail);

/* 事件上报回调：开锁/拒绝/锁定时调用，由 main 接到 MQTT 发布（safe/log） */
typedef void (*auth_event_cb_t)(const char *evt, const char *user, const char *detail, int res);

void auth_fsm_init(void);
void auth_fsm_set_ui_hook(auth_ui_hook_t hook);
void auth_fsm_set_event_cb(auth_event_cb_t cb);

void auth_fsm_tick(void);                         /* 主线程 100ms 调用：轮询 hal_face + 状态超时 */

fsm_state_t      auth_fsm_state(void);
const char *     auth_fsm_state_name(fsm_state_t s);
int              auth_fsm_last_score(void);
const char *     auth_fsm_pending_user(void);
int              auth_fsm_lock_remaining(void);    /* 0 = 无锁定 */

void auth_fsm_submit_detect(int face_id, int score);   /* 注入一次识别结果（HAL→FSM） */
void auth_fsm_submit_pin(const char * pin);            /* PIN 通道入口（主页键盘） */
void auth_fsm_submit_otp(const char * code);           /* 动态码页提交 */
void auth_fsm_cancel_otp(void);

void auth_fsm_note_unlock(const char * user);          /* 其它通道成功后统一走此处（脉冲+日志+UI） */
void auth_fsm_note_failure(const char * detail);       /* 失败计数 + 设备级锁定判定 */

#ifdef __cplusplus
} /*extern "C"*/
#endif
