/**
 * @file lv_font_cn_decl.h
 * 声明工程自带的中文字体，让 LVGL 的默认字体配置也能引用到它们。
 *
 * 这个文件是被 lv_conf.h 里的自定义字体配置自动包含的，不是给业务代码用的，
 * 所以不用在别处 include。
 *
 * 字体文件本身是脚本生成的（tools/fonts/gen_fonts.py），不要手改。
 */
#ifndef LV_FONT_CN_DECL_H
#define LV_FONT_CN_DECL_H

#define LV_FONT_CUSTOM_DECLARE                              \
    LV_FONT_DECLARE(lv_font_cn_14)                          \
    LV_FONT_DECLARE(lv_font_cn_16)                          \
    LV_FONT_DECLARE(lv_font_cn_20)                          \
    LV_FONT_DECLARE(lv_font_cn_28)

#endif /*LV_FONT_CN_DECL_H*/
