/**
 * @file theme.c
 * 主题实现（DESIGN.md §6）。
 */
#include "theme.h"
#include <string.h>

/* ---------------- 四套色板（DESIGN.md §6.1 原表） ---------------- */
#define TH_COL(bg, panel, panel2, text, tmut, placeholder, border, acc, acc_ink, ok, warn, danger) \
    { { LV_COLOR_MAKE((bg>>16)&0xFF,(bg>>8)&0xFF,bg&0xFF), \
        LV_COLOR_MAKE((panel>>16)&0xFF,(panel>>8)&0xFF,panel&0xFF), \
        LV_COLOR_MAKE((panel2>>16)&0xFF,(panel2>>8)&0xFF,panel2&0xFF), \
        LV_COLOR_MAKE((text>>16)&0xFF,(text>>8)&0xFF,text&0xFF), \
        LV_COLOR_MAKE((tmut>>16)&0xFF,(tmut>>8)&0xFF,tmut&0xFF), \
        LV_COLOR_MAKE((placeholder>>16)&0xFF,(placeholder>>8)&0xFF,placeholder&0xFF), \
        LV_COLOR_MAKE((border>>16)&0xFF,(border>>8)&0xFF,border&0xFF), \
        LV_COLOR_MAKE((acc>>16)&0xFF,(acc>>8)&0xFF,acc&0xFF), \
        LV_COLOR_MAKE((acc_ink>>16)&0xFF,(acc_ink>>8)&0xFF,acc_ink&0xFF), \
        LV_COLOR_MAKE((ok>>16)&0xFF,(ok>>8)&0xFF,ok&0xFF), \
        LV_COLOR_MAKE((warn>>16)&0xFF,(warn>>8)&0xFF,warn&0xFF), \
        LV_COLOR_MAKE((danger>>16)&0xFF,(danger>>8)&0xFF,danger&0xFF) } }

const app_theme_t THEMES[THEME_COUNT] = {
    /* 石墨黑（深色默认）: bg/panel/elevated/text/text2/dim/border/accent/ink/ok/warn/danger */
    { "石墨黑", TH_COL(0x0D1117, 0x161B22, 0x1C2330, 0xE6EDF3, 0x8B98A5, 0x5B6671,
                       0x30363D, 0x0091FF, 0xFFFFFF, 0x31A24C, 0xF2A918, 0xE41E3F) },
    /* 月白（暖光浅色） */
    { "月白",   TH_COL(0xF5F3EF, 0xFFFAF7, 0xECE8E1, 0x2B2722, 0x6B6358, 0x9A9186,
                       0xDCD6CC, 0xC0892D, 0xFFFFFF, 0x2E7D4F, 0xC8860A, 0xC0392B) },
    /* 蓝白（冷色浅色） */
    { "蓝白",   TH_COL(0xEEF4FB, 0xFFFFFF, 0xDDE8F5, 0x1A2B45, 0x5A6B85, 0x8A99B0,
                       0xD5DEE8, 0x1565D8, 0xFFFFFF, 0x1F9254, 0xD98A00, 0xD32F2F) },
    /* 松石青（深色青绿） */
    { "松石青", TH_COL(0x0A1F1C, 0x102A26, 0x163A34, 0xE3F2EE, 0x8FB3AB, 0x5F8079,
                       0x1A3D3A, 0x1BB3A0, 0xFFFFFF, 0x2FAE8A, 0xE0A52E, 0xE5533D) },
    /* 浅蓝（浅色天蓝，用户偏好）：accent=Light Blue 400，柔和明亮不刺眼 */
    { "浅蓝",   TH_COL(0xF0F7FD, 0xFFFFFF, 0xE1F0FB, 0x14324C, 0x5C7A93, 0x9DB4C6,
                       0xD2E4F2, 0x29B6F6, 0xFFFFFF, 0x2E9E6B, 0xE8A33D, 0xE5564B) },
};

/* ---------------- 全局样式 ---------------- */
lv_style_t st_screen;
lv_style_t st_panel;
lv_style_t st_panel2;
lv_style_t st_text;
lv_style_t st_text_mut;
lv_style_t st_text_placeholder;
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

static int g_idx = 1;   /* 默认主题索引：0 石墨黑 / 1 月白 / 2 蓝白 / 3 松石青 / 4 浅蓝 */
static lv_style_transition_dsc_t s_trans;  /* 按钮按下过渡描述符（apply_theme 内初始化） */

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
    lv_style_set_radius(&st_panel, 16);
    lv_style_set_pad_all(&st_panel, 16);
    /* 柔和投影： elevace 观感（浅蓝主题下更明显） */
    lv_style_set_shadow_color(&st_panel, lv_color_hex(0x16344C));
    lv_style_set_shadow_width(&st_panel, 24);
    lv_style_set_shadow_opa(&st_panel, 20);
    lv_style_set_shadow_ofs_y(&st_panel, 4);

    lv_style_set_bg_color(&st_panel2, p->c[TH_PANEL2]);
    lv_style_set_bg_opa(&st_panel2, LV_OPA_COVER);
    lv_style_set_radius(&st_panel2, 12);
    lv_style_set_pad_all(&st_panel2, 10);

    lv_style_set_text_color(&st_text, p->c[TH_TEXT]);
    lv_style_set_text_color(&st_text_mut, p->c[TH_TEXT_MUT]);
    lv_style_set_text_color(&st_text_placeholder, p->c[TH_TEXT_PLACEHOLDER]);

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

    /* 底部 Tab：未选中 = 透明底 + 次要文字（胶囊页签底） */
    lv_style_set_bg_opa(&st_tab_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&st_tab_btn, p->c[TH_TEXT_MUT]);
    lv_style_set_border_width(&st_tab_btn, 0);
    lv_style_set_pad_all(&st_tab_btn, 8);
    /* 选中 = 强调色实心胶囊 + 反白文字（设计稿 4 胶囊页签） */
    lv_style_set_text_color(&st_tab_btn_checked, p->c[TH_ACCENT_INK]);
    lv_style_set_bg_color(&st_tab_btn_checked, p->c[TH_ACCENT]);
    lv_style_set_bg_opa(&st_tab_btn_checked, LV_OPA_COVER);
    lv_style_set_border_width(&st_tab_btn_checked, 0);
    lv_style_set_pad_all(&st_tab_btn_checked, 8);

    /* 交互过渡：按钮/页签按下平滑变色（美观增强，150ms，v9 过渡描述符 API） */
    static const lv_style_prop_t trans_props[] = { LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR, 0 };
    lv_style_transition_dsc_init(&s_trans, trans_props, NULL, 150, 0, NULL);
    lv_style_set_transition(&st_accent_btn, &s_trans);
    lv_style_set_transition(&st_ghost_btn, &s_trans);
    lv_style_set_transition(&st_danger_btn, &s_trans);
    lv_style_set_transition(&st_tab_btn, &s_trans);
}

void theme_init(void)
{
    /* 每个样式只初始化一次（后续 theme_switch 只改内容） */
    lv_style_init(&st_screen);
    lv_style_init(&st_panel);
    lv_style_init(&st_panel2);
    lv_style_init(&st_text);
    lv_style_init(&st_text_mut);
    lv_style_init(&st_text_placeholder);
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
    g_idx = 4;          /* 默认主题：浅蓝（用户偏好浅蓝色系） */
    apply_theme();
}

/* ---- 主题切换回调（最多 4 个） ---- */
#define THEME_CB_MAX 4
static theme_change_cb_t s_cbs[THEME_CB_MAX];
static int s_cb_cnt = 0;

void theme_register_change_cb(theme_change_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < s_cb_cnt; i++) if (s_cbs[i] == cb) return;  /* 去重 */
    if (s_cb_cnt >= THEME_CB_MAX) return;
    s_cbs[s_cb_cnt++] = cb;
}

void theme_switch(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_idx = idx;
    apply_theme();
    lv_obj_report_style_change(NULL);   /* 全局重绘，页面无需重建 */
    /* 通知回调，刷新那些用了本地颜色覆盖的控件（顶栏胶囊/锁图标/WiFi 等） */
    for (int i = 0; i < s_cb_cnt; i++) {
        if (s_cbs[i]) s_cbs[i](idx);
    }
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


