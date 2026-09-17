/**
 * @file page_keypad.h
 * 密码键盘页：3×4 数字矩阵 + 掩码输入框 + 虚位密码校验反馈。
 */
#pragma once

#include "lvgl.h"
#include "core/auth/auth_fsm.h"   /* fsm_state_t：向键盘页回填异步校验结果 */

#ifdef __cplusplus
extern "C" {
#endif

/* 在指定父容器内创建本页面，返回页面根容器 */
lv_obj_t * page_keypad_create(lv_obj_t * parent);

/* 回填 PIN 异步校验结果（v1.9）。由 ui.c 的 fsm_ui_hook 在主线程调用，
 * 且仅当键盘页当前可见时调用：用于显示「校验中」及失败提示。
 * 成功路径由 fsm_ui_hook 直接切回主页，不经过此处。 */
void page_keypad_on_fsm(fsm_state_t st, const char *detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif
