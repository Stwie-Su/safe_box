/**
 * @file page_otp.h
 * 动态密码页（DESIGN.md §7.1 双因子第二步）：人脸置信度落在 [mid,high) 时，
 * FSM 进入 WAIT_OTP，UI 切到此页让用户输入 TOTP（RFC 6238，6 位，30s 窗口）。
 */
#pragma once

#include "lvgl.h"
#include "core/auth/auth_fsm.h"   /* fsm_state_t：page_otp_on_fsm 的签名需要 */

#ifdef __cplusplus
extern "C" {
#endif

/* 在指定父容器内创建本页面，返回页面根容器 */
lv_obj_t * page_otp_create(lv_obj_t * parent);

/* FSM 状态回填：动态码校验在 worker 线程完成，结果经 ui.c 的 fsm_ui_hook
 * 派发回主线程后由本函数就地更新提示文案（与 page_keypad_on_fsm 同款）。
 * 仅当本页当前可见时被调用；成功 / 锁定由 hook 切回主页。 */
void page_otp_on_fsm(fsm_state_t st, const char *detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif
