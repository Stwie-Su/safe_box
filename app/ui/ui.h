/**
 * @file ui.h
 * 视图层对外接口（前端外壳）—— 智能保险柜（DESIGN.md §5）。
 *
 * 信息架构（底部四胶囊页签：主页 / 用户 / 日志 / 设置）：
 *   主页（锁状态 · 时钟 · 开锁占位）
 *   用户（用户管理一级页，敏感操作仍需 admin PIN）
 *   日志（记录一级页，随时审计）
 *   设置（进前二次验证 admin PIN）
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
LV_FONT_DECLARE(lv_font_cn_14)
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)
LV_FONT_DECLARE(lv_font_cn_28)

/* 页面 ID：底部 Tab 为前三个，其余为设置子页 / 全屏层 */
typedef enum {
    PAGE_HOME     = 0,   /* 主页（锁状态 + 时钟 + 开锁占位） */
    PAGE_LOGS,           /* 日志（独立一级页） */
    PAGE_SETTINGS,       /* 开发者选项（只读诊断，无鉴权；FR-7 2026-09-14 变更） */
    PAGE_USERS,          /* 用户管理（底部「用户」一级页签） */
    PAGE_NETWORK,        /* 网络（主导航一级入口） */
    PAGE_SYSTEM,         /* 系统（主导航一级入口：策略/主题/恢复出厂） */
    PAGE_KEYPAD,         /* 开锁 PIN 键盘（主页全屏层） */
    PAGE_OTP,            /* 动态密码（人脸置信度二次确认，全屏层） */
    PAGE_FACE,           /* 人脸识别（全屏层：从主页"人脸识别"卡进入；视频区留空） */
    PAGE_COUNT
} ui_page_t;

/* 应用入口：初始化主题/存储并创建整个 UI 外壳。由 main.c 调用。 */
void ui_init(void);

/* 切换到指定页面（隐藏其余容器、显示目标页，不重建）。 */
void ui_switch_page(ui_page_t page);

/* 图标+文本按钮辅助函数：左侧放 LV_SYMBOL_*，右侧放 CJK 文本。
 * icon 用 Montserrat（含 LVGL 符号），text 用当前主题中文字体；
 * 适用于需要同时显示符号和中文的按钮（返回/保存/添加等）。 */
lv_obj_t * ui_icon_text_button(lv_obj_t * parent,
                               const char * icon,
                               const char * text,
                               int32_t w, int32_t h,
                               lv_style_t * style,
                               lv_color_t text_color,
                               lv_event_cb_t cb,
                               void * user_data);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SAFE_UI_H*/
