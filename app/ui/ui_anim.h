/**
 * @file ui_anim.h
 * UI 动效常量集中地（UI 现代化 spec §2 / §3 / §5 / §6）。
 *
 * 目的：所有时长 / 位移 / 缓动 / 染色停留时长只在本文件出现一次，
 * 页面与主题代码一律引用这里的宏，禁止在业务代码散落魔法数字，
 * 便于在单核 Cortex-A7（1024×600）上按实测帧率统一调参。
 *
 * 性能预算（spec §6，A7 单核）：
 *   - 单段动画 ≤ 250ms；页面过渡 200ms 入场 / 160ms 退场（两段错峰，非叠加全屏重绘）
 *   - 只动 translate / opa / 颜色这类样式属性，不销毁重建对象
 *   - 禁用 blur_backdrop、大 shadow_width（>20）、全屏波纹、lv_obj_set_style_anim
 *   - 动画期间不分配内存（不使用每帧 malloc 的路径）
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 页面切换过渡（spec §3） ----------------
 * 横向位移 + 淡入淡出，方向随左 rail 上下选择而变：
 *   新页：translate_x ±46px → 0 + opa 0 → 255，UI_TRANS_MS(200) ease-out
 *   旧页：translate_x 0 → ∓46px + opa 255 → 0，UI_TRANS_OUT_MS(160) ease-out
 * 位移取 46px（视觉真源 ui_redesign_preview.html 的 translateX(46px)）：
 * 既看得出方向，又不至于让旧页大面积滑出 content 区造成额外重绘。
 * 退场比入场快：两段动画错峰，避免同时占满重绘带宽（A7 单核）。
 *
 * 调参口径：若板端实测 HOME 稳态 fps 降幅 >15%，先降 UI_TRANS_MS 到 160，
 * 仍不达标则退化为 opacity-only（把 UI_TRANS_SHIFT_PX 设为 0）。 */
#define UI_TRANS_MS          200     /* 新页入场时长（ease-out） */
#define UI_TRANS_OUT_MS      160     /* 旧页退场时长（略快） */
#define UI_TRANS_SHIFT_PX    46      /* 页面切换横向位移基准（设计稿 px） */
#define UI_TRANS_EASE        lv_anim_path_ease_out

/* 旧版纵向过渡的缓动宏：主题层（theme.c 的 transition dsc）仍在使用，
 * 保留以免改动主题动效；页面过渡已改用上面的 UI_TRANS_*。 */
#define UI_ANIM_PAGE_EASE_IN     lv_anim_path_ease_in
#define UI_ANIM_PAGE_EASE_OUT    lv_anim_path_ease_out

/* ---------------- 顶部横幅（spec §1 / §6） ----------------
 * 淡入 250ms → 停留 hold_ms（≤2s）→ 淡出 250ms；横幅是 ~64px 高小条。 */
#define UI_ANIM_BANNER_FADE_MS   250     /* 淡入 / 淡出单段时长 */
#define UI_ANIM_BANNER_HOLD_MS   2000    /* 默认停留（OK / DANGER 档） */
#define UI_ANIM_BANNER_HOLD_MS_S 1500    /* 短停留（WARN / INFO 档） */
#define UI_ANIM_BANNER_H_BASE    64      /* 横条基准高度（设计稿 px） */

/* ---------------- 按压手感（spec §3 / §6） ----------------
 * 纯样式过渡（LVGL 内建 transition），不额外创建 anim 对象。
 * 按压 120ms 进、回弹 180ms 出——进快出慢，符合「按下即响应」的直觉。 */
#define UI_ANIM_PRESS_IN_MS      120
#define UI_ANIM_PRESS_OUT_MS     180
#define UI_ANIM_PRESS_SHRINK_W   (-6)    /* 按压态横向微缩（两侧合计 12px） */
#define UI_ANIM_PRESS_SHRINK_H   (-3)    /* 按压态纵向微缩（两侧合计 6px） */
/* ★ 帧率 + 现代化（2026-09-22）：**按钮阴影全部取消**（宽度置 0）。
 *   ① 帧率：users / system 页有 60+ 个按钮，每个都要额外画一层模糊阴影
 *      （模糊半径 8px × 按钮周长），纯 CPU 软件渲染下是白扔的固定开销；
 *   ② 现代化：当下主流是**扁平化** —— 控件不再靠投影表达层级，改由
 *      「填充色 + 1px 边框 + 表面色差」承担。卡片（st_panel，16px 阴影）
 *      仍保留投影，层级落在**大容器**上，而不是每个小按钮上；
 *   ③ 按压反馈不丢失：st_btn_press 的微缩 transform 与配色仍在，
 *      且比阴影更即时。
 *   ⚠️ 要恢复只需把下面两行的 W 改回非零（Y / OPA 随之生效）。 */
#define UI_ANIM_PRESS_SHADOW_W   0       /* 按压态阴影宽度（0 = 不画） */
#define UI_ANIM_PRESS_SHADOW_Y   0
#define UI_ANIM_PRESS_SHADOW_OPA 0
#define UI_ANIM_IDLE_SHADOW_W    0       /* 常态阴影宽度（0 = 不画） */
#define UI_ANIM_IDLE_SHADOW_Y    0
#define UI_ANIM_IDLE_SHADOW_OPA  0

/* ---------------- 导航高亮渐变（spec §3） ----------------
 * 左 rail 的图标 / 文字 / 页面切换共用：颜色 150ms 渐变。 */
#define UI_ANIM_TAB_MS           150

/* ---------------- 人脸扫描框状态染色（spec §3 后半） ----------------
 * FAIL / WARN 是瞬时反馈，停留 600ms 后自动回 IDLE，避免颜色长期滞留；
 * OK 态由下一次识别结果或离开人脸页结束。 */
#define UI_ANIM_SCAN_FAIL_MS     600

#ifdef __cplusplus
} /*extern "C"*/
#endif
