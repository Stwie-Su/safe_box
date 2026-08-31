/**
 * @file page_monitor.h
 * 综合监控页（首页）：人脸预览占位 + 安防状态 + 快捷操作 + 最近日志。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_monitor_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
