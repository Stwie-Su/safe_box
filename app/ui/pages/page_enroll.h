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

/* 录入结果在**本页内**展示（用户拍板 2026-09-15：录入失败不要弹 modal 对话框）。
 *   text：结果文案（如「录入超时：没有检测到人脸，请正对镜头」）；传 NULL = 清除
 *         结果态，底部状态行交回状态定时器托管、按钮恢复成「取消录入」。
 *   ok  ：true = 成功（成功色），false = 失败（告警色，按钮变「重新录入」）。
 * 由 page_users 的 on_face_event_ui 在收到 FACE_EV_ENROLL_DONE 后调用。 */
void page_enroll_show_result(const char * text, bool ok);

#ifdef __cplusplus
}
#endif
