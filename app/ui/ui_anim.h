/**
 * @file ui_anim.h
 * 动画参数集中放这里：时长、位移、缓动、停留时间。
 *
 * 为什么集中：调快慢时只改这一处，页面代码里不要出现散落的数字。
 * 板子是单核 CPU、纯软件渲染，动画越长越容易掉帧，所以这里的值都取得比较短。
 *
 * 几条经验：
 *   - 单段动画尽量不超过 250ms；
 *   - 只改位置、透明度、颜色这类属性，不要为了动画去销毁重建控件；
 *   - 别用大面积模糊和长阴影，那是纯 CPU 的固定开销。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 页面切换：新页滑入 + 淡入，旧页滑出 + 淡出。
 * 退场比入场快一点，两段错开，避免同时重绘拖慢帧率。
 * 板端实测如果帧率掉超过 15%，先缩短时长，再把位移设为 0（只做淡入淡出）。 */
#define UI_TRANS_MS          60      /* 新页入场时长（实测 200ms 会掉帧，降到 60） */
#define UI_TRANS_OUT_MS      40      /* 旧页退场时长 */
#define UI_TRANS_SHIFT_PX    12      /* 横向滑动距离（原来 46，缩小减少重绘面积） */
#define UI_TRANS_EASE        lv_anim_path_ease_out

/* 主题里还在用的旧缓动名，保留着不动。 */
#define UI_ANIM_PAGE_EASE_IN     lv_anim_path_ease_in
#define UI_ANIM_PAGE_EASE_OUT    lv_anim_path_ease_out

/* 顶部提示条：淡入 → 停留 → 淡出。 */
#define UI_ANIM_BANNER_FADE_MS   250     /* 淡入 / 淡出各自时长 */
#define UI_ANIM_BANNER_HOLD_MS   2000    /* 默认停留 2 秒 */
#define UI_ANIM_BANNER_HOLD_MS_S 1500    /* 次要提示停留 1.5 秒 */
#define UI_ANIM_BANNER_H_BASE    64      /* 提示条高度 */

/* 按钮按下效果：按下快、回弹慢，手感更跟手。
 * 阴影全部关掉（宽度 0）：页面上按钮很多，每个都画阴影在单核上负担很重，
 * 而且现在的界面风格本来就是扁平化，按压靠缩小 + 变色表达就够了。 */
#define UI_ANIM_PRESS_IN_MS      120
#define UI_ANIM_PRESS_OUT_MS     180
#define UI_ANIM_PRESS_SHRINK_W   (-6)    /* 按下时左右各缩 3px */
#define UI_ANIM_PRESS_SHRINK_H   (-3)    /* 按下时上下各缩 1.5px */
#define UI_ANIM_PRESS_SHADOW_W   0       /* 0 = 不画阴影 */
#define UI_ANIM_PRESS_SHADOW_Y   0
#define UI_ANIM_PRESS_SHADOW_OPA 0
#define UI_ANIM_IDLE_SHADOW_W    0
#define UI_ANIM_IDLE_SHADOW_Y    0
#define UI_ANIM_IDLE_SHADOW_OPA  0

/* 左侧导航高亮渐变时长。 */
#define UI_ANIM_TAB_MS           150

/* 人脸扫描框变色后停留多久回待机（失败/警告是瞬时提示）。 */
#define UI_ANIM_SCAN_FAIL_MS     600

#ifdef __cplusplus
} /*extern "C"*/
#endif
