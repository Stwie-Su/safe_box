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

#ifdef __cplusplus
} /*extern "C"*/
#endif
