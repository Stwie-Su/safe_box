/**
 * @file page_settings.h
 * 开发者选项页：只看诊断信息，不做业务操作。
 * 进它下面的子页之前要先输管理员 PIN。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_settings_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
