/**
 * @file page_users.h
 * 用户管理页（DESIGN.md §7.3）：用户列表 + 添加；点用户 → 详情（改密 / 删除 / 启用）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_users_create(lv_obj_t * parent);

/* 重新发起一次人脸录入（录入页失败态「重新录入」按钮用）。
 * 前置：本页曾发起过录入（s_last_enroll_uid >= 0）。成功返回 true；
 * 模组忙/后端不受理返回 false（调用方据此提示用户稍后再试）。 */
bool page_users_retry_enroll(void);

/* 测试钩子：仅 PC 调试用，由 main.c 的 SAFE_TEST_DLG 调用 */
void page_users_test_open_add_dlg(void);
void page_users_test_open_auth_dlg(void);
void page_users_test_open_change_pwd_dlg(void);
void page_users_test_open_otp_dlg(void);
void page_users_test_open_face_dlg(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

