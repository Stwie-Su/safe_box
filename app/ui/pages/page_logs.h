/**
 * @file page_logs.h
 * 日志页：显示开箱记录、告警、用户操作记录，可以按类型筛选。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_logs_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
