/**
 * @file page_keypad.h
 * 输密码页（全屏）：3×4 数字键盘 + 掩码输入框 + 校验结果提示。
 *
 * 由 ui_init 建一次，之后只切显示/隐藏，不重建。
 */
#pragma once

#include "lvgl.h"
#include "core/auth/auth_fsm.h"   /* fsm_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_keypad_create(lv_obj_t * parent);

/* 把校验结果回填到页面（显示「校验中」或失败原因）。
 * 由 ui.c 的状态回调在主线程调，且只在本页可见时调；成功时界面会直接切回主页，不走这里。 */
void page_keypad_on_fsm(fsm_state_t st, const char *detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif
