/**
 * @file ui_scale.h
 * 屏幕适配：设计稿按 1024×600 画，实际分辨率不同就按比例缩放。
 *
 * 页面里写尺寸时套一层 SX()/SY()，例如 SX(120)，
 * 在 1024 宽的屏上就是 120，在更宽的屏上会等比例放大。
 */
#pragma once

#include "lvgl/lvgl.h"
#include "fonts/fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BASE_WIDTH  1024
#define UI_BASE_HEIGHT 600

/* 初始化，读当前屏幕尺寸算比例。开机调一次。 */
void ui_scale_init(void);

/* 当前横向 / 纵向缩放比例，以及两者中较小的那个。 */
float ui_scale_x(void);
float ui_scale_y(void);
float ui_scale_min(void);

/* 设计稿像素 → 实际像素。 */
#define SX(px)  ((int32_t)((px) * ui_scale_x()))
#define SY(px)  ((int32_t)((px) * ui_scale_y()))

/* 按缩放比例挑一个最合适的中文字体。 */
const lv_font_t * app_font_scaled(uint16_t base_size);

/* 挑一个最合适的图标字体。图标字体只有 14/18/22 几档，
 * 而且里面没有中文字符，所以不能和上面那个混用。 */
const lv_font_t * app_montserrat_scaled(uint16_t base_size);

#ifdef __cplusplus
}
#endif
