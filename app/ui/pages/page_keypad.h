/**
 * @file page_keypad.h
 * 密码键盘页：3×4 数字矩阵 + 掩码输入框 + 虚位密码校验反馈。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在指定父容器内创建本页面，返回页面根容器 */
lv_obj_t * page_keypad_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
