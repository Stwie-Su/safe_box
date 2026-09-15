/**
 * @file page_enroll.h
 * 人脸录入全屏页（FR-19/FR-27 细化）：录入专用窗口，与「人脸识别」页分开
 * （用户拍板 2026-09-15：录入只管录入，识别只管识别）。
 *
 * 进入：用户页发起录入 → ui_switch_page(PAGE_ENROLL)；
 * 退出：ENROLL_DONE（成功/失败/30s 兜底超时）自动回用户页，或「取消录入」。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_enroll_create(lv_obj_t * parent);

/* 本页当前是否可见（duty-cycle 前台判定用：人脸录入页前台 = 相机与模组工作）。
 * page_face 的前台边沿会把本页可见性并入（两个窗口共用模组与相机）。 */
bool page_enroll_is_visible(void);

#ifdef __cplusplus
}
#endif
