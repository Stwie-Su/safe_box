/**
 * @file page_otp.h
 * 动态码页（全屏）：人脸连续没认出来时，让用户输手机上的 6 位动态码。
 *
 * 有效期 30 秒一个时间片，界面会显示倒计时。
 */
#pragma once

#include "lvgl.h"
#include "core/auth/auth_fsm.h"   /* fsm_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_otp_create(lv_obj_t * parent);

/* 把校验结果回填到页面提示文字。
 * 校验在后台线程做，结果回到主线程后由 ui.c 调这里；只在本页可见时调。 */
void page_otp_on_fsm(fsm_state_t st, const char *detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif
