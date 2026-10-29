/**
 * @file fonts.h
 * 字体的统一入口。页面里一律写 app_font(16) 这种，不直接引用具体字体变量。
 *
 * 两种环境两种做法：
 *   PC  —— 用 FreeType 从系统里加载中文字体，不怕缺字；
 *   板子 —— 用提前生成好的位图字体（lv_font_cn_*），随固件编进去。
 * 这样界面代码两边完全一样。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#if LV_USE_FREETYPE

/* PC：运行时加载的字体指针，由 fonts_ft.c 初始化。 */
extern lv_font_t * g_app_font_14;
extern lv_font_t * g_app_font_16;
extern lv_font_t * g_app_font_20;
extern lv_font_t * g_app_font_28;
extern lv_font_t * g_app_font_36;
extern lv_font_t * g_app_font_44;

#define app_font(size) g_app_font_##size

#else

/* 板子：编译进固件的位图字体。 */
LV_FONT_DECLARE(lv_font_cn_14)
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)
LV_FONT_DECLARE(lv_font_cn_28)
#define app_font(size) (&lv_font_cn_##size)

#endif /* LV_USE_FREETYPE */

/* 初始化字体。要在建任何文字控件之前调（ui_init 最前面）。 */
void app_fonts_init(void);

#ifdef __cplusplus
}
#endif
