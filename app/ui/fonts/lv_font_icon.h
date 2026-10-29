/**
 * @file lv_font_icon.h
 * 图标字体（Material Icons 里常用的一批，转成位图编进固件）。
 *
 * 为什么不用 TTF 动态加载：板子没开 FreeType，运行时加载的字体在板子上没有，
 * 会出现「电脑上图标正常、板子上一片空白」。而且板子没网，
 * 图标必须跟着固件一起烧进去。
 *
 * 四档字号 16/20/24/32，由 lv_font_conv 生成，总共约 195KB。
 * 选字号时往大了取：小图标放大比大图标缩小更难看，所以不往下缩。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern lv_font_t lv_font_icon_16;
extern lv_font_t lv_font_icon_20;
extern lv_font_t lv_font_icon_24;
extern lv_font_t lv_font_icon_32;

#ifdef __cplusplus
} /*extern "C"*/
#endif
