/**
 * @file ui_feedback.h
 * UI 层反馈中枢：把总线事件翻译成「顶部横幅 + 人脸扫描框染色」（spec §1 + §3 后半）。
 *
 * 设计要点：
 *  - 横幅挂在 ui.c 的常驻 overlay 上（跨页存活）——LOCKOUT 时 FSM 会强制切回
 *    主页，若各页面自订订阅会丢事件，故集中到本模块；
 *  - 只经 event_bus 订阅（EV_FACE_EVENT / EV_AUTH_RESULT），不 include core 内部头；
 *  - 颜色一律来自 theme_color()，无写死 hex（CLAUDE.md §8 色彩铁律）；
 *  - 动效参数全部取自 ui_anim.h（A7 单核预算：横幅 ~64px 高小条，单段 ≤250ms）。
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 横幅档位 → 主题色（spec §1：BANNER_INFO=accent / OK / WARN / DANGER） */
typedef enum {
    UI_BANNER_INFO   = 0,   /* 提示：accent */
    UI_BANNER_OK     = 1,   /* 成功：TH_OK */
    UI_BANNER_WARN   = 2,   /* 警示：TH_WARN */
    UI_BANNER_DANGER = 3,   /* 危险：TH_DANGER */
    UI_BANNER_LEVEL_COUNT
} ui_banner_level_t;

/**
 * @brief 初始化反馈中枢并在 host 上建好常驻横幅（默认隐藏）。
 *
 * 触发时机：ui_init() 末尾、所有页面实例化之后（页面已建好，扫描框可染色）。
 * @param host  横幅宿主容器（ui.c 的 overlay：位于顶栏之下、内容区之上的浮层）
 */
void ui_feedback_init(lv_obj_t * host);

/**
 * @brief 显示一条瞬时横幅：淡入 → 停留 hold_ms → 淡出 → 隐藏。
 *
 * 新横幅到达时删掉旧动画、直接替换（不排队、不叠加）。
 * @param text     文案（NULL 视为空串）
 * @param level    档位（决定主题色）
 * @param hold_ms  停留时长（建议 ≤ UI_ANIM_BANNER_HOLD_MS）
 */
void ui_banner(const char * text, ui_banner_level_t level, uint32_t hold_ms);

/**
 * @brief 显示一条常驻横幅（只淡入，不自动淡出），供 LOCKOUT 倒计时每秒刷新文案。
 *        必须由 ui_banner_hide() 结束。
 */
void ui_banner_sticky(const char * text, ui_banner_level_t level);

/**
 * @brief 结束当前横幅（淡出后隐藏）。对瞬时横幅也安全（会先删掉它的动画）。
 */
void ui_banner_hide(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
