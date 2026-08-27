/**
 * @file page_network.h
 * 网络页（DESIGN.md §7.5）：WiFi 扫描列表 + 已存网络 + 连接输 PSK。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_network_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
