
/**
 * @file ui_scale.h
 * UI adaptive scaling system.
 */
#pragma once

#include "lvgl/lvgl.h"
#include "fonts/fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BASE_WIDTH  1024
#define UI_BASE_HEIGHT 600

void ui_scale_init(void);
float ui_scale_x(void);
float ui_scale_y(void);
float ui_scale_min(void);

#define SX(px)  ((int32_t)((px) * ui_scale_x()))
#define SY(px)  ((int32_t)((px) * ui_scale_y()))

const lv_font_t * app_font_scaled(uint16_t base_size);

/* 离散档位 Montserrat：按 s_scale_min 选最接近的 14/18/22 之一，用于键盘符号等
 * 不能用 FreeType 动态渲染的场合（FreeType 字体没 LVGL 符号）。 */
const lv_font_t * app_montserrat_scaled(uint16_t base_size);

#ifdef __cplusplus
}
#endif

