/**
 * @file page_settings.h
 * 设置中枢页（DESIGN.md §7.2）：用户管理 / 网络 / 系统 入口。
 * 进入任一子页前需二次验证 admin PIN（DESIGN.md §2.1）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_settings_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
