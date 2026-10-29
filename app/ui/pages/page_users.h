/**
 * @file page_users.h
 * 用户管理页：列出所有用户，可以添加；点某个用户进详情（改密码 / 删除 / 启用）。
 *
 * 敏感操作都要先输管理员 PIN 或动态码。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_users_create(lv_obj_t * parent);

/* 重新发起一次人脸录入（录入页「重新录入」按钮用）。
 * 前提是这个页面之前发起过一次录入。成功返回 true，模组忙返回 false。 */
bool page_users_retry_enroll(void);

/* 测试用：直接打开某个弹窗，只有 PC 调试（SAFE_TEST_DLG）才调用。 */
void page_users_test_open_add_dlg(void);
void page_users_test_open_auth_dlg(void);
void page_users_test_open_change_pwd_dlg(void);
void page_users_test_open_otp_dlg(void);
void page_users_test_open_face_dlg(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
