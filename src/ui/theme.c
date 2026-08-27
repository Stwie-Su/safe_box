/**
 * @file theme.c
 * 主题实现（DESIGN.md §6）。
 */
#include "theme.h"
#include <string.h>

/* ---------------- 四套色板（DESIGN.md §6.1 原表） ---------------- */
#define TH_COL(bg, panel, panel2, text, tmut, border, acc, acc_ink, ok, warn, danger) \
    { { LV_COLOR_MAKE((bg>>16)&0xFF,(bg>>8)&0xFF,bg&0xFF), \
        LV_COLOR_MAKE((panel>>16)&0xFF,(panel>>8)&0xFF,panel&0xFF), \
        LV_COLOR_MAKE((panel2>>16)&0xFF,(panel2>>8)&0xFF,panel2&0xFF), \
        LV_COLOR_MAKE((text>>16)&0xFF,(text>>8)&0xFF,text&0xFF), \
        LV_COLOR_MAKE((tmut>>16)&0xFF,(tmut>>8)&0xFF,tmut&0xFF), \
        LV_COLOR_MAKE((border>>16)&0xFF,(border>>8)&0xFF,border&0xFF), \
        LV_COLOR_MAKE((acc>>16)&0xFF,(acc>>8)&0xFF,acc&0xFF), \
        LV_COLOR_MAKE((acc_ink>>16)&0xFF,(acc_ink>>8)&0xFF,acc_ink&0xFF), \
        LV_COLOR_MAKE((ok>>16)&0xFF,(ok>>8)&0xFF,ok&0xFF), \
        LV_COLOR_MAKE((warn>>16)&0xFF,(warn>>8)&0xFF,warn&0xFF), \
        LV_COLOR_MAKE((danger>>16)&0xFF,(danger>>8)&0xFF,danger&0xFF) } }

const app_theme_t THEMES[THEME_COUNT] = {
    { "石墨黑", TH_COL(0x14161A, 0x1E2127, 0x262A31, 0xE9EBEE, 0x9AA0A8,
                       0x33383F, 0xC8A45C, 0x1A1407, 0x5FB37A, 0xD9A441, 0xD2584F) },
    { "月白",   TH_COL(0xF3F0EA, 0xFFFFFF, 0xEDE9E1, 0x1B1D21, 0x6E6A62,
                       0xDED8CD, 0xB08D3E, 0xFFFFFF, 0x3F8F5C, 0xB5862C, 0xB23A33) },
    { "蓝白",   TH_COL(0xF2F6FB, 0xFFFFFF, 0xE9F0F8, 0x16222E, 0x6B7B8C,
                       0xD5DEE8, 0x2D6FB3, 0xFFFFFF, 0x3F8F5C, 0xC0882A, 0xC0493F) },
    { "松石青", TH_COL(0x0F1719, 0x16242A, 0x1E3036, 0xE2EEF0, 0x8AA0A4,
                       0x2A3D43, 0x3FB6A8, 0x06201D, 0x5FB37A, 0xD9A441, 0xD2584F) },
};

/* ---------------- 全局样式 ---------------- */
lv_style_t st_screen;
lv_style_t st_panel;
lv_style_t st_panel2;
lv_style_t st_text;
lv_style_t st_text_mut;
lv_style_t st_border;
lv_style_t st_accent_btn;
lv_style_t st_accent_btn_pr;
lv_style_t st_ghost_btn;
lv_style_t st_danger_btn;
lv_style_t st_ok_text;
lv_style_t st_warn_text;
lv_style_t st_danger_text;
lv_style_t st_tab_btn;
lv_style_t st_tab_btn_checked;

static int g_idx = 0;

/* 依据当前主题刷新全部样式内容 */
static void apply_theme(void)
{
    const app_theme_colors_t *p = &THEMES[g_idx].pal;

    lv_style_set_bg_color(&st_screen, p->c[TH_BG]);
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);

    lv_style_set_bg_color(&st_panel, p->c[TH_PANEL]);
    lv_style_set_bg_opa(&st_panel, LV_OPA_COVER);
    lv_style_set_border_color(&st_panel, p->c[TH_BORDER]);
    lv_style_set_border_width(&st_panel, 1);
    lv_style_set_radius(&st_panel, 12);
    lv_style_set_pad_all(&st_panel, 16);

    lv_style_set_bg_color(&st_panel2, p->c[TH_PANEL2]);
    lv_style_set_bg_opa(&st_panel2, LV_OPA_COVER);
    lv_style_set_radius(&st_panel2, 8);
    lv_style_set_pad_all(&st_panel2, 10);

    lv_style_set_text_color(&st_text, p->c[TH_TEXT]);
    lv_style_set_text_color(&st_text_mut, p->c[TH_TEXT_MUT]);

    lv_style_set_border_color(&st_border, p->c[TH_BORDER]);
    lv_style_set_border_width(&st_border, 1);
    lv_style_set_radius(&st_border, 6);

    lv_style_set_bg_color(&st_accent_btn, p->c[TH_ACCENT]);
    lv_style_set_bg_opa(&st_accent_btn, LV_OPA_COVER);
    lv_style_set_text_color(&st_accent_btn, p->c[TH_ACCENT_INK]);
    lv_style_set_radius(&st_accent_btn, 10);
    lv_style_set_border_width(&st_accent_btn, 0);
    lv_style_set_pad_hor(&st_accent_btn, 20);
    lv_style_set_pad_ver(&st_accent_btn, 10);

    lv_style_set_bg_color(&st_accent_btn_pr, lv_color_darken(p->c[TH_ACCENT], LV_OPA_30));
    lv_style_set_text_color(&st_accent_btn_pr, p->c[TH_ACCENT_INK]);
    lv_style_set_radius(&st_accent_btn_pr, 10);
    lv_style_set_border_width(&st_accent_btn_pr, 0);
    lv_style_set_pad_hor(&st_accent_btn_pr, 20);
    lv_style_set_pad_ver(&st_accent_btn_pr, 10);

    lv_style_set_bg_color(&st_ghost_btn, p->c[TH_PANEL]);
    lv_style_set_text_color(&st_ghost_btn, p->c[TH_TEXT]);
    lv_style_set_border_color(&st_ghost_btn, p->c[TH_BORDER]);
    lv_style_set_border_width(&st_ghost_btn, 1);
    lv_style_set_radius(&st_ghost_btn, 10);
    lv_style_set_pad_hor(&st_ghost_btn, 20);
    lv_style_set_pad_ver(&st_ghost_btn, 10);

    lv_style_set_bg_color(&st_danger_btn, p->c[TH_DANGER]);
    lv_style_set_text_color(&st_danger_btn, lv_color_white());
    lv_style_set_radius(&st_danger_btn, 10);
    lv_style_set_border_width(&st_danger_btn, 0);
    lv_style_set_pad_hor(&st_danger_btn, 20);
    lv_style_set_pad_ver(&st_danger_btn, 10);

    lv_style_set_text_color(&st_ok_text, p->c[TH_OK]);
    lv_style_set_text_color(&st_warn_text, p->c[TH_WARN]);
    lv_style_set_text_color(&st_danger_text, p->c[TH_DANGER]);

    /* 底部 Tab：未选中 = 透明底 + 次要文字 */
    lv_style_set_bg_color(&st_tab_btn, p->c[TH_PANEL]);
    lv_style_set_bg_opa(&st_tab_btn, LV_OPA_COVER);
    lv_style_set_text_color(&st_tab_btn, p->c[TH_TEXT_MUT]);
    lv_style_set_border_width(&st_tab_btn, 0);
    lv_style_set_pad_all(&st_tab_btn, 8);
    /* 选中 = 强调色文字 + 顶部细条 */
    lv_style_set_text_color(&st_tab_btn_checked, p->c[TH_ACCENT]);
    lv_style_set_bg_color(&st_tab_btn_checked, p->c[TH_PANEL]);
    lv_style_set_border_width(&st_tab_btn_checked, 0);
    lv_style_set_pad_all(&st_tab_btn_checked, 8);
}

void theme_init(void)
{
    /* 每个样式只初始化一次（后续 theme_switch 只改内容） */
    lv_style_init(&st_screen);
    lv_style_init(&st_panel);
    lv_style_init(&st_panel2);
    lv_style_init(&st_text);
    lv_style_init(&st_text_mut);
    lv_style_init(&st_border);
    lv_style_init(&st_accent_btn);
    lv_style_init(&st_accent_btn_pr);
    lv_style_init(&st_ghost_btn);
    lv_style_init(&st_danger_btn);
    lv_style_init(&st_ok_text);
    lv_style_init(&st_warn_text);
    lv_style_init(&st_danger_text);
    lv_style_init(&st_tab_btn);
    lv_style_init(&st_tab_btn_checked);
    g_idx = 0;
    apply_theme();
}

void theme_switch(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_idx = idx;
    apply_theme();
    lv_obj_report_style_change(NULL);   /* 全局重绘，页面无需重建 */
}

int theme_idx(void)
{
    return g_idx;
}

const char * theme_name(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return "";
    return THEMES[idx].name;
}

lv_color_t theme_color(theme_role_t role)
{
    if (role < 0 || role >= TH_ROLE_MAX) role = TH_TEXT;
    return THEMES[g_idx].pal.c[role];
}
