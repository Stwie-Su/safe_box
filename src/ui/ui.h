/**
 * @file ui.h
 * 视图层对外接口（前端外壳）—— 智能保险柜（DESIGN.md §5）。
 *
 * 信息架构：
 *   主界面（锁状态 · 时钟 · 开锁占位）
 *    ├─ 日志 · 记录（独立一级页，随时审计）
 *    └─ 设置（进前二次验证 admin PIN）
 *         ├─ 用户管理 / 网络 / 系统
 *
 * 分层约定：ui/ 只做「界面渲染 + 事件绑定」；业务判定一律走 src/core/；
 * 硬件动作一律走 src/hal/。
 */
#ifndef SAFE_UI_H
#define SAFE_UI_H

#include "lvgl.h"
#include "fonts/fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 嵌入中文字体声明（开发板阶段使用；PC 阶段由 fonts.h 的 app_font() 切换到 FreeType） */
LV_FONT_DECLARE(lv_font_cn_14);
LV_FONT_DECLARE(lv_font_cn_16);
LV_FONT_DECLARE(lv_font_cn_20);
LV_FONT_DECLARE(lv_font_cn_28);

/* 页面 ID：底部 Tab 为前三个，其余为设置子页 / 全屏层 */
typedef enum {
    PAGE_HOME     = 0,   /* 主页（锁状态 + 时钟 + 开锁占位） */
    PAGE_LOGS,           /* 日志（独立一级页） */
    PAGE_SETTINGS,       /* 设置中枢（二次验证 admin PIN） */
    PAGE_USERS,          /* 用户管理（设置子页） */
    PAGE_NETWORK,        /* 网络（设置子页） */
    PAGE_SYSTEM,         /* 系统（设置子页） */
    PAGE_KEYPAD,         /* 开锁 PIN 键盘（主页全屏层） */
    PAGE_COUNT
} ui_page_t;

/* 应用入口：初始化主题/存储并创建整个 UI 外壳。由 main.c 调用。 */
void app_start(void);

/* 切换到指定页面（隐藏其余容器、显示目标页，不重建）。 */
void ui_switch_page(ui_page_t page);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SAFE_UI_H*/
