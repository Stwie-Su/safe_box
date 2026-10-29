/**
 * @file auth_fsm.h
 * 人脸 / 密码开锁的「大脑」：接收识别结果和按键，决定开不开锁。
 *
 * 处理流程：
 *   人脸结果 → 按原因分流 → 开锁 / 转输动态码 / 拒绝 / 锁定
 *   其中「未匹配」会累计次数：攒够几次转动态码，再攒够几次锁定。
 *
 * 调用关系：
 *   - 主线程每 100ms 调 auth_fsm_tick()，由它推进超时、检查人脸结果；
 *   - 界面调 auth_fsm_submit_pin() / submit_otp() 提交输入；
 *   - 状态一变就通过 ui_hook 通知界面切页，通过 event_cb 通知日志 / 远程上报。
 *   本模块不包含 LVGL，界面层的东西一律通过回调出去。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "hal/hal_face.h"   /* face_reason_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSM_IDLE = 0,     /* 待机 */
    FSM_DETECTING,    /* 识别中 */
    FSM_UNLOCKED,     /* 已开锁（界面上展示 3 秒） */
    FSM_WAIT_OTP,     /* 等用户输入动态码（60 秒超时） */
    FSM_DENY,         /* 拒绝（展示 2 秒） */
    FSM_LOCKOUT,      /* 锁定中（倒计时） */
    FSM_VERIFYING,    /* PIN 正在后台校验，界面显示「校验中」 */
} fsm_state_t;

/* 状态变化时通知界面（切页 / 弹窗 / 提示）。user 和 detail 可能为 NULL。 */
typedef void (*auth_ui_hook_t)(fsm_state_t st, const char *user, const char *detail);

/* 开锁 / 拒绝 / 锁定时通知外部，用来写日志和上报远程通道。 */
typedef void (*auth_event_cb_t)(const char *evt, const char *user, const char *detail, int res);

/* 开机初始化一次（要排在 ui_init 之前，界面才能收到第一条状态）。 */
void auth_fsm_init(void);

/* 注册上面两个回调。ui_hook 在 ui_init 里设，event_cb 在 app_main 里设。 */
void auth_fsm_set_ui_hook(auth_ui_hook_t hook);
void auth_fsm_set_event_cb(auth_event_cb_t cb);

/* 主线程每 100ms 调一次：检查人脸结果、推进各种超时。 */
void auth_fsm_tick(void);

/* 查询当前状态 / 状态名 / 上一次人脸失败原因。 */
fsm_state_t      auth_fsm_state(void);
const char *     auth_fsm_state_name(fsm_state_t s);
face_reason_t    auth_fsm_last_reason(void);

/* 当前正在验证的用户名（没人时返回 NULL）。 */
const char *     auth_fsm_pending_user(void);

/* 锁定还剩几秒（0 = 没锁）。 */
int              auth_fsm_lock_remaining(void);

/* 连续失败次数（界面显示「未匹配（2/3）」用）。 */
int              auth_fsm_fail_streak(void);

/* 注入一次人脸识别结果（由人脸模块的轮询结果转过来）。 */
void auth_fsm_submit_face(int32_t face_id, face_reason_t reason);

/* 同上，额外带模组报回来的用户名，用来核对「这张脸是不是真的属于这个人」。
 * mod_name 为空表示模组没给名字（老模板），这时跳过核对，不能判失效。 */
void auth_fsm_submit_face_ex(int32_t face_id, face_reason_t reason, const char * mod_name);

/* 提交一个 PIN。**异步**：马上返回，真正的比对在后台线程做，
 * 校验结果会通过状态迁移体现（所以调用后不要立刻读 auth_fsm_state 判断成败）。
 * 校验期间重复提交会被忽略。 */
void auth_fsm_submit_pin(const char * pin);

/* 设置 PIN 校验的兜底超时（秒，默认 120）。只给测试用，正常路径不需要调。 */
void auth_fsm_set_verify_timeout(uint32_t seconds);

/* 提交一个动态码。同样异步，提交后仍停在 WAIT_OTP，等结果再迁移。 */
void auth_fsm_submit_otp(const char * code);

/* 取消动态码，回到待机。 */
void auth_fsm_cancel_otp(void);

/* 别的通道（远程 / 人脸）开锁成功后统一调这里：动作、日志、界面一次做完。 */
void auth_fsm_note_unlock(const char * user);

/* 记一次失败（累计次数，够了就锁定）。 */
void auth_fsm_note_failure(const char * detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif
