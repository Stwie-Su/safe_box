/**
 * @file theme.h
 * 主题（配色）：五套颜色，一键切换，所有控件跟着变。
 *
 * 规矩：颜色的十六进制值只写在这个文件里，页面代码一律用「颜色角色」
 * （背景 / 面板 / 正文 / 强调 / 危险……）或者直接用下面这些样式。
 * 这样换主题时不用改任何页面，以后加主题也只要在这里加一组颜色。
 *
 * 切换时刷新的是样式内容，不是重建控件，所以换肤是瞬间的。
 */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_COUNT 5

/* 颜色角色：页面里说「我要正文色」，而不是写死 #333333。 */
typedef enum {
    TH_BG,          /* 页面背景 */
    TH_PANEL,       /* 卡片 / 面板 */
    TH_PANEL2,      /* 次一级面板（输入区、列表项） */
    TH_TEXT,        /* 正文 */
    TH_TEXT_MUT,    /* 次要文字 */
    TH_TEXT_PLACEHOLDER, /* 输入框提示文字 */
    TH_BORDER,      /* 边框 / 分隔线 */
    TH_ACCENT,      /* 强调色（主按钮、选中态） */
    TH_ACCENT_INK,  /* 强调色上的文字 */
    TH_OK,          /* 成功 */
    TH_WARN,        /* 警告 */
    TH_DANGER,      /* 危险 / 失败 */
    TH_ROLE_MAX
} theme_role_t;

typedef struct {
    lv_color_t c[TH_ROLE_MAX];
} app_theme_colors_t;

typedef struct {
    const char * name;      /* 显示名，例如「石墨黑」 */
    app_theme_colors_t pal;
} app_theme_t;

/* 五套主题，顺序就是主题编号。 */
extern const app_theme_t THEMES[THEME_COUNT];

/* ---- 全局样式：页面直接拿来用，不要自己再写颜色 ---- */
extern lv_style_t st_screen;        /* 屏幕背景 */
extern lv_style_t st_panel;         /* 卡片 */
extern lv_style_t st_panel2;        /* 次面板 */
extern lv_style_t st_text;          /* 正文 */
extern lv_style_t st_text_mut;      /* 次要文字 */
extern lv_style_t st_text_placeholder; /* 输入框提示 */
extern lv_style_t st_border;        /* 描边 / 分隔 */
extern lv_style_t st_accent_btn;    /* 主按钮 */
extern lv_style_t st_accent_btn_pr; /* 主按钮按下 */
extern lv_style_t st_ghost_btn;     /* 次要按钮 */
extern lv_style_t st_danger_btn;    /* 危险按钮 */
extern lv_style_t st_ok_text;       /* 成功色文字 */
extern lv_style_t st_warn_text;     /* 警告色文字 */
extern lv_style_t st_danger_text;   /* 危险色文字 */
extern lv_style_t st_tab_btn;       /* 底部页签（未选中） */
extern lv_style_t st_tab_btn_checked; /* 底部页签（选中） */
extern lv_style_t st_btn_press;     /* 按钮按下效果，自动挂到所有按钮上 */
extern lv_style_t st_tab_hl;        /* 页签高亮的渐变载体 */

/* 初始化样式，套用默认主题。开机调一次（ui_init 里）。 */
void theme_init(void);

/* 切到第 idx 套主题：更新样式、整屏重绘、通知所有注册过的回调。 */
void theme_switch(int idx);

/* 主题切换回调：给那些自己设过颜色（没走样式）的控件用，
 * 它们要在切主题时手动刷新一下。最多注册 4 个。 */
typedef void (*theme_change_cb_t)(int idx);

/* 注册回调。**返回 false 表示槽位满了、这次没注册上**，
 * 调用方要据此知道自己的刷新不会被触发。 */
bool theme_register_change_cb(theme_change_cb_t cb);

/* 当前是第几套 / 第几套叫什么名字。 */
int theme_idx(void);
const char * theme_name(int idx);

/* 按角色取颜色。只有画布这类必须直接给颜色的地方才用，普通控件请用样式。 */
lv_color_t theme_color(theme_role_t role);

/* 给所有按钮装上「按下缩小 + 变色」的效果（包括以后动态创建的按钮）。
 * theme_init 里会自动调一次，重复调没关系。 */
void theme_press_install(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
