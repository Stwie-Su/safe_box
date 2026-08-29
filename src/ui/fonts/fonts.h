/**
 * @file fonts.h
 * 字体统一入口（DESIGN.md §4 / CLAUDE.md）。
 *
 * 设计要点：
 *  - PC 阶段用 FreeType 从系统 Noto CJK 动态渲染，避免嵌入位图字体缺字；
 *  - 开发板阶段用预烧录的 lv_font_cn_* 位图字体（后续重新生成完整字库）；
 *  - UI 代码统一写 app_font(14/16/20/28)，不直接引用具体字体变量。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#if LV_USE_FREETYPE

/* PC：FreeType 运行时字体指针，由 fonts_ft.c 初始化 */
extern lv_font_t * g_app_font_14;
extern lv_font_t * g_app_font_16;
extern lv_font_t * g_app_font_20;
extern lv_font_t * g_app_font_28;
extern lv_font_t * g_app_font_36;
extern lv_font_t * g_app_font_44;

#define app_font(size) g_app_font_##size

#else

/* 开发板：使用预烧录位图字体 */
LV_FONT_DECLARE(lv_font_cn_14)
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)
LV_FONT_DECLARE(lv_font_cn_28)
#define app_font(size) (&lv_font_cn_##size)

#endif /* LV_USE_FREETYPE */

/* 初始化字体系统（app_start 中先于任何 label 创建调用） */
void app_fonts_init(void);

#ifdef __cplusplus
}
#endif
