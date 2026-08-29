
#include "ui_scale.h"

static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static float s_scale_min = 1.0f;

void ui_scale_init(void)
{
    lv_display_t * disp = lv_display_get_default();
    if (!disp) { s_scale_x = s_scale_y = s_scale_min = 1.0f; return; }
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w <= 0) w = UI_BASE_WIDTH;
    if (h <= 0) h = UI_BASE_HEIGHT;
    s_scale_x = (float)w / (float)UI_BASE_WIDTH;
    s_scale_y = (float)h / (float)UI_BASE_HEIGHT;
    s_scale_min = (s_scale_x < s_scale_y) ? s_scale_x : s_scale_y;
}

float ui_scale_x(void) { return s_scale_x; }
float ui_scale_y(void) { return s_scale_y; }
float ui_scale_min(void) { return s_scale_min; }

const lv_font_t * app_font_scaled(uint16_t base_size)
{
    uint16_t sz = (uint16_t)(base_size * s_scale_min + 0.5f);
    if (sz < 10) sz = 10;
    if (sz > 60) sz = 60;
    sz = (sz / 2) * 2;
#if LV_USE_FREETYPE
    if (sz <= 14) return g_app_font_14;
    if (sz <= 18) return g_app_font_16;
    if (sz <= 24) return g_app_font_20;
    return g_app_font_28;
#else
    if (sz <= 14) return &lv_font_cn_14;
    if (sz <= 18) return &lv_font_cn_16;
    if (sz <= 24) return &lv_font_cn_20;
    return &lv_font_cn_28;
#endif
}

/* ★ 离散档位 Montserrat 选字号（用于键盘等「纯 ASCII + LVGL 符号」的场景）。
 *
 * 为什么键盘不能用中文字体：lv_keyboard 的控制键是 LV_SYMBOL_*(⌫ ↩ ⏎ ⇧)，
 * 这些码位只存在于 Montserrat，Noto CJK / 嵌入中文字体里没有 → 套中文字体会变方块。
 * 所以键盘整块（字母、数字、符号）统一走 Montserrat，字号按缩放放大。
 *
 * 档位用 #if LV_FONT_MONTSERRAT_xx 守卫：defconfig 没开的字号不会编译进来，
 * 开发板可以只开小档位省 Flash，PC 全开。
 */
const lv_font_t * app_montserrat_scaled(uint16_t base_size)
{
    float effective = s_scale_min * (float)base_size;
#if LV_FONT_MONTSERRAT_48
    if (effective >= 45.0f) return &lv_font_montserrat_48;
#endif
#if LV_FONT_MONTSERRAT_44
    if (effective >= 41.0f) return &lv_font_montserrat_44;
#endif
#if LV_FONT_MONTSERRAT_40
    if (effective >= 37.0f) return &lv_font_montserrat_40;
#endif
#if LV_FONT_MONTSERRAT_36
    if (effective >= 33.0f) return &lv_font_montserrat_36;
#endif
#if LV_FONT_MONTSERRAT_32
    if (effective >= 29.0f) return &lv_font_montserrat_32;
#endif
#if LV_FONT_MONTSERRAT_28
    if (effective >= 26.0f) return &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_24
    if (effective >= 23.0f) return &lv_font_montserrat_24;
#endif
#if LV_FONT_MONTSERRAT_22
    if (effective >= 20.5f) return &lv_font_montserrat_22;
#endif
#if LV_FONT_MONTSERRAT_20
    if (effective >= 19.0f) return &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_18
    if (effective >= 16.5f) return &lv_font_montserrat_18;
#endif
#if LV_FONT_MONTSERRAT_16
    if (effective >= 15.0f) return &lv_font_montserrat_16;
#endif
    return &lv_font_montserrat_14;
}

