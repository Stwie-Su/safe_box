/**
 * @file theme.h
 * 主题系统（DESIGN.md §6）：四套色板 + 全局复用样式 + 一键切换。
 *
 * 实现要点：
 *  - 颜色集中：THEMES[4]，页面控件只引用 lv_style_t，绝不写死 hex；
 *  - 切主题 theme_switch(idx) 更新样式内容后 lv_obj_report_style_change(NULL) 全局刷新，
 *    页面无需重建即可换肤；
 *  - 加主题只需在 THEMES[] 增一行。
 */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_COUNT 5

/* 颜色角色（DESIGN.md §6.1 色板列） */
typedef enum {
    TH_BG,          /* 页面背景 */
    TH_PANEL,       /* 面板 */
    TH_PANEL2,      /* 次面板 */
    TH_TEXT,        /* 正文 */
    TH_TEXT_MUT,    /* 次要文字 */
    TH_TEXT_PLACEHOLDER, /* 输入框占位/提示文字 */
    TH_BORDER,      /* 描边 */
    TH_ACCENT,      /* 强调 */
    TH_ACCENT_INK,  /* 强调上文字 */
    TH_OK,          /* 成功 */
    TH_WARN,        /* 警示 */
    TH_DANGER,      /* 危险 */
    TH_ROLE_MAX
} theme_role_t;

typedef struct {
    lv_color_t c[TH_ROLE_MAX];
} app_theme_colors_t;

typedef struct {
    const char * name;      /* 显示名：石墨黑 / 月白 / 蓝白 / 松石青 */
    app_theme_colors_t pal;
} app_theme_t;

/* 四套主题（DESIGN.md §6.1，顺序即枚举） */
extern const app_theme_t THEMES[THEME_COUNT];

/* ---- 全局复用样式（DESIGN.md §6.2） ---- */
extern lv_style_t st_screen;        /* 屏幕背景 */
extern lv_style_t st_panel;         /* 卡片面板 */
extern lv_style_t st_panel2;        /* 次面板（输入区/列表项） */
extern lv_style_t st_text;          /* 正文 */
extern lv_style_t st_text_mut;      /* 次要文字 */
extern lv_style_t st_text_placeholder; /* 输入框占位/提示文字 */
extern lv_style_t st_border;        /* 描边/分隔 */
extern lv_style_t st_accent_btn;    /* 强调按钮 */
extern lv_style_t st_accent_btn_pr; /* 强调按钮按下 */
extern lv_style_t st_ghost_btn;     /* 次要按钮 */
extern lv_style_t st_danger_btn;    /* 危险按钮 */
extern lv_style_t st_ok_text;       /* 成功色文字 */
extern lv_style_t st_warn_text;     /* 警示色文字 */
extern lv_style_t st_danger_text;   /* 危险色文字 */
extern lv_style_t st_tab_btn;       /* 底部 Tab（未选中） */
extern lv_style_t st_tab_btn_checked; /* 底部 Tab（选中） */
extern lv_style_t st_btn_press;     /* 全局按压态（theme 钩子自动挂到所有 lv_button） */
extern lv_style_t st_tab_hl;        /* 页签高亮渐变载体（只带 transition，无颜色） */

/* 初始化样式并应用默认主题（月白）。app_start 调用一次。 */
void theme_init(void);

/* 切换到第 idx 套主题（0..THEME_COUNT-1）：更新样式 + 全局重绘 + 通知所有注册回调 */
void theme_switch(int idx);

/* 注册主题切换回调（用于刷新那些用 lv_obj_set_style_*_color() 设本地颜色覆盖的控件）。
 * 同一个回调重复注册会被忽略；切换主题时按注册顺序调用，最多 4 个。 */
typedef void (*theme_change_cb_t)(int idx);
void theme_register_change_cb(theme_change_cb_t cb);

/* 当前主题索引 / 名称 */
int theme_idx(void);
const char * theme_name(int idx);

/* 便捷取色（canvas 绘制等特殊场景；普通控件请用样式） */
lv_color_t theme_color(theme_role_t role);

/**
 * @brief 安装全局按压手感主题钩子（UI 现代化 spec §3 前半）。
 *
 * 用 LVGL 的 theme apply 回调给【每一个】lv_button 挂上 st_btn_press
 * （LV_STATE_PRESSED），后续动态创建的按钮也自动生效——调用方无需逐个接线。
 * theme_init() 末尾自动调用一次；重复调用幂等。
 */
void theme_press_install(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif


