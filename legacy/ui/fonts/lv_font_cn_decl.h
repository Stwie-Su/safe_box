/**
 * @file lv_font_cn_decl.h
 * 自定义嵌入字体声明头（LVGL 官方自定义字体机制）。
 *
 * 通过 lv_conf.h 中的 LV_FONT_USE_CUSTOM_INCLUDE / LV_FONT_CUSTOM_INCLUDE
 * 被 lv_conf_internal.h 引入，其内的 LV_FONT_CUSTOM_DECLARE 会被
 * lvgl/font/lv_font.h 展开为 extern 声明，从而让 LV_FONT_DEFAULT 等
 * 位置（如 lv_display.c 的主题默认字体）能正确引用本工程字体。
 *
 * 字体文件本体由 tools/fonts/gen_fonts.py 生成，勿手改。
 */
#ifndef LV_FONT_CN_DECL_H
#define LV_FONT_CN_DECL_H

#define LV_FONT_CUSTOM_DECLARE                              \
    LV_FONT_DECLARE(lv_font_cn_14)                          \
    LV_FONT_DECLARE(lv_font_cn_16)                          \
    LV_FONT_DECLARE(lv_font_cn_20)                          \
    LV_FONT_DECLARE(lv_font_cn_28)

#endif /*LV_FONT_CN_DECL_H*/
