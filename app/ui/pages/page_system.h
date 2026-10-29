/**
 * @file page_system.h
 * 系统页：时间日期、安全策略、主题切换、恢复出厂。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_system_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
