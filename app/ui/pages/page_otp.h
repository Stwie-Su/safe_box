/**
 * @file page_otp.h
 * 动态密码页（DESIGN.md §7.1 双因子第二步）：人脸置信度落在 [mid,high) 时，
 * FSM 进入 WAIT_OTP，UI 切到此页让用户输入 TOTP（RFC 6238，6 位，30s 窗口）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在指定父容器内创建本页面，返回页面根容器 */
lv_obj_t * page_otp_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
