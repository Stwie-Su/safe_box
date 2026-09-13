/**
 * @file lv_font_icon.h
 * 图标字体（Material Icons 子集，由 lv_font_conv 子集化生成）。
 *
 * 为什么是「编译进位图字体」而不是 FreeType 加载 TTF：
 *  - 开发板**不启用 FreeType**（见 fonts_ft.c：板子走 lv_font_cn_*.c 位图字体），
 *    FreeType 路径只在 PC 生效，图标会「PC 有、板子无」；
 *  - 图标资源必须随固件编译进去（板子无网络、部署不额外拷字体文件）。
 *
 * 生成参数（与工程既有 lv_font_cn_*.c 同约定，见各 .c 文件头注释）：
 *   lv_font_conv --size {16|20|24|32} --bpp 4 --format lvgl \
 *       --font MaterialIcons-Regular.ttf -r <40 个码位> \
 *       --no-compress --no-prefilter --force-fast-kern-format
 * 选 40 个码位约 195KB（16/20/24/32 四档），覆盖导航、卡片、列表行、按钮与状态图标。
 *
 * 字号分档：设计稿图标像素 16~34px，取 16/20/24/32 四档，按「向上取最近档」
 * 选择——小图标放大比大图标缩小更失真，故不跨档缩小。
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

