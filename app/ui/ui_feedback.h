/**
 * @file ui_feedback.h
 * 界面反馈：收到事件后弹顶部提示条、给人脸扫描框换颜色。
 *
 * 为什么单独一个文件：提示条要跨页面存在。比如锁定倒计时时状态机会强制切回主页，
 * 如果各页面自己管提示，切页就把提示丢了。集中在这里就不会丢。
 *
 * 它只订阅事件总线（人脸结果、开锁结果），不直接调业务代码。
 * 颜色全部来自主题，动画参数全部来自 ui_anim.h。
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 提示条档位：决定颜色（提示 / 成功 / 警告 / 危险）。 */
typedef enum {
    UI_BANNER_INFO   = 0,
    UI_BANNER_OK     = 1,
    UI_BANNER_WARN   = 2,
    UI_BANNER_DANGER = 3,
    UI_BANNER_LEVEL_COUNT
} ui_banner_level_t;

/* 初始化，并在 host 上建好提示条（默认隐藏）。
 * 在 ui_init() 最后调，那时页面都建好了，扫描框才能跟着染色。
 * host 是覆盖在内容区上方的那层容器。 */
void ui_feedback_init(lv_obj_t * host);

/* 弹一条提示：淡入 → 停留 hold_ms → 淡出。
 * 新提示会直接替换掉旧的（不排队、不叠加）。 */
void ui_banner(const char * text, ui_banner_level_t level, uint32_t hold_ms);

/* 弹一条不会自动消失的提示（锁定倒计时用它，每秒刷新文案）。
 * 必须自己调 ui_banner_hide() 结束。 */
void ui_banner_sticky(const char * text, ui_banner_level_t level);

/* 收起提示条。瞬时提示也可以安全调用。 */
void ui_banner_hide(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
