/**
 * @file page_monitor.h
 * 主页：显示锁状态、时间、人脸预览占位、快捷操作和最近几条日志。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_monitor_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
