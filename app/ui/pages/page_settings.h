/**
 * @file page_settings.h
 * 开发者选项页（FR-7，2026-09-14 变更）：只读诊断信息，不含业务功能。
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
