/**
 * @file auth_fsm.h
 * 人脸认证状态机（FR-2 按原因分流 / 核心）。
 *
 * 这是人脸开锁链路的「大脑」：接收 hal_face 的识别结果（face_reason_t 原因码，
 * FM225 模组不输出分数），按原因分流 → UNLOCK / WAIT_OTP / DENY，
 * 并负责失败计数与设备级锁定。
 *
 * 分流规则（需求 v1.6 FR-19）：
 *   OK           → 开锁；
 *   NO_MATCH     → 计数，连续达 face_otp_after 转 WAIT_OTP，再达 max_failed 锁定；
 *   LIVENESS_FAIL→ 拒绝 + 防伪告警；
 *   TIMEOUT/ERROR→ 不计数。
 *
 * 与 UI 解耦：状态变化通过 auth_ui_hook 回调通知 UI 层（切页、弹窗、提示），
 * 不直接 #include LVGL（NFR-5：业务层不依赖 UI / 平台头文件）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "hal/hal_face.h"   /* face_reason_t（core→hal 为架构允许方向） */

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
face_reason_t    auth_fsm_last_reason(void);       /* 取代旧 last_score（FM225 无分数） */
const char *     auth_fsm_pending_user(void);
int              auth_fsm_lock_remaining(void);    /* 0 = 无锁定 */

/* 设备级连续失败计数（只读查询，规约 §5.2）。
 * UI 现代化 ui1 新增：横幅「未匹配（n/face_otp_after）」需要展示当前连败数，
 * 而 fail_streak 原为 auth_fsm.c 内部静态、未暴露。本接口只读取，不修改状态；
 * 成功开锁与进入 LOCKOUT 时该计数由状态机自行清零。 */
int              auth_fsm_fail_streak(void);

void auth_fsm_submit_face(int32_t face_id, face_reason_t reason);  /* 注入一次人脸结果（HAL→FSM） */
void auth_fsm_submit_pin(const char * pin);            /* PIN 通道入口（主页键盘） */
void auth_fsm_submit_otp(const char * code);           /* 动态码页提交 */
void auth_fsm_cancel_otp(void);

void auth_fsm_note_unlock(const char * user);          /* 其它通道成功后统一走此处（脉冲+日志+UI） */
void auth_fsm_note_failure(const char * detail);       /* 失败计数 + 设备级锁定判定 */

#ifdef __cplusplus
} /*extern "C"*/
#endif

