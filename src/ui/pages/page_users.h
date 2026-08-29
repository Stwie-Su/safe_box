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

/* 测试钩子：仅 PC 调试用，由 main.c 的 SAFE_TEST_DLG 调用 */
void page_users_test_open_add_dlg(void);
void page_users_test_open_auth_dlg(void);
void page_users_test_open_change_pwd_dlg(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

