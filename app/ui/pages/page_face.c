/**
 * @file page_face.c
 * 人脸识别全屏页（v2）：
 *   - 顶部：返回按钮 + 标题「人脸识别」
 *   - 中央：方形「视频预览占位」面板（暂无摄像头时显示提示文字），四
 *          角扫描框 + 一束光带从上方周期下扫（lv_anim 动画）
 *   - 底部：状态文字「请将面部对准摄像头」 + 取消按钮
 *
 * 设计约束：
 *   - 摄像头未配置：显示提示文字「摄像头未配置」；
 *   - 摄像头已配置但未连接：显示「正在连接摄像头…」+ 持续扫描光带；
 *   - 配置完成后真实画面由 app/hal/camera 提供，本阶段仅留空 UI 框架。
 */
#include "page_face.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "core/auth/auth_fsm.h"
#include <stdlib.h>
#include <string.h>

/* ===== 静态对象句柄 ===== */
static lv_obj_t * s_scan_beam;        // 顶部扫描光带（细长矩形）

/* ----- 私有声明 ----- */
static void mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
                      int32_t w, int32_t h, lv_color_t color);
static void back_cb(lv_event_t * e);
static void cancel_cb(lv_event_t * e);
static void beam_anim_xcb(void * obj, int32_t v);
static lv_anim_t s_beam_anim;

/**
 * @brief 在指定位置画一个填充矩形（L 形角的一段）
 */
static void mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
                      int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, MAX(w, h) / 2, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
}

/**
 * @brief 创建人脸识别页
 */
lv_obj_t * page_face_create(lv_obj_t * parent)
{
    /* 根容器 */
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, SX(20), 0);
    lv_obj_set_style_pad_row(root, SY(18), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ---------- 顶部 bar：返回 + 标题 ---------- */
    lv_obj_t * top = lv_obj_create(root);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_outline_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, SX(12), 0);

    /* 「← 返回」图标 + 文字按钮（避免纯图标被理解为「向前」/「取消」之类） */
    lv_obj_t * back = lv_button_create(top);
    lv_obj_set_size(back, LV_SIZE_CONTENT, SY(40));
    lv_obj_set_style_radius(back, SX(20), 0);
    lv_obj_set_style_bg_color(back, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_outline_width(back, 0, 0);
    lv_obj_set_style_pad_left(back, SX(14), 0);
    lv_obj_set_style_pad_right(back, SX(16), 0);
    lv_obj_set_style_pad_top(back, 0, 0);
    lv_obj_set_style_pad_bottom(back, 0, 0);
    lv_obj_set_style_pad_column(back, SX(8), 0);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    ui_icon_create(back, UI_ICON_BACK, SX(20), theme_color(TH_TEXT));
    lv_obj_t * back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_color(back_lbl, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(back_lbl, app_font_scaled(15), 0);

    /* 中间标题 */
    lv_obj_t * title = lv_label_create(top);
    lv_label_set_text(title, "人脸识别");
    lv_obj_set_style_text_color(title, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, app_font_scaled(20), 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* 右侧占位（与左侧对称） */
    lv_obj_t * right = lv_obj_create(top);
    lv_obj_set_size(right, SX(40), SY(40));
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_outline_width(right, 0, 0);

    /* ---------- 中央：视频预览占位面板（固定尺寸便于 4 角扫描框定位） ---------- */
    int32_t prev_w = SY(360);
    int32_t prev_h = SY(280);
    lv_obj_t * preview = lv_obj_create(root);
    lv_obj_set_size(preview, prev_w, prev_h);
    lv_obj_add_style(preview, &st_panel, 0);
    lv_obj_set_style_radius(preview, SX(18), 0);
    lv_obj_set_style_bg_color(preview, theme_color(TH_PANEL), 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_outline_width(preview, 0, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);    /* 不要 st_panel 那个 16px pad */

    /* 占位文字（无摄像头） */
    lv_obj_t * ph_label = lv_label_create(preview);
    lv_label_set_text(ph_label, "视频预览区域\n（摄像头未配置）");
    lv_obj_set_style_text_color(ph_label, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_font(ph_label, app_font_scaled(15), 0);
    lv_obj_set_style_text_align(ph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ph_label);

    /* 4 角扫描框（用 4 个 L 形组合，每个角由两段矩形构成） */
    int32_t pad = SX(20);
    int32_t len = SX(36);
    int32_t thick = SY(4);

    /* 左上角 */
    mk_corner(preview, pad, pad, len, thick, theme_color(TH_ACCENT));
    mk_corner(preview, pad, pad, thick, len, theme_color(TH_ACCENT));
    /* 右上角 */
    mk_corner(preview, prev_w - pad - len, pad, len, thick, theme_color(TH_ACCENT));
    mk_corner(preview, prev_w - pad - thick, pad, thick, len, theme_color(TH_ACCENT));
    /* 左下角 */
    mk_corner(preview, pad, prev_h - pad - thick, len, thick, theme_color(TH_ACCENT));
    mk_corner(preview, pad, prev_h - pad - len, thick, len, theme_color(TH_ACCENT));
    /* 右下角 */
    mk_corner(preview, prev_w - pad - len, prev_h - pad - thick, len, thick, theme_color(TH_ACCENT));
    mk_corner(preview, prev_w - pad - thick, prev_h - pad - len, thick, len, theme_color(TH_ACCENT));

    /* 扫描光带（占宽小于内框） */
    s_scan_beam = lv_obj_create(preview);
    int32_t beam_w = prev_w - 2 * pad - SX(80);
    int32_t beam_h = SY(3);
    lv_obj_set_size(s_scan_beam, beam_w, beam_h);
    lv_obj_set_pos(s_scan_beam, pad + SX(40), pad);
    lv_obj_set_style_bg_color(s_scan_beam, theme_color(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_scan_beam, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_scan_beam, beam_h / 2, 0);
    lv_obj_set_style_border_width(s_scan_beam, 0, 0);
    lv_obj_set_style_outline_width(s_scan_beam, 0, 0);

    /* ---------- 底部：状态文字 + 取消按钮 ---------- */
    lv_obj_t * status = lv_label_create(root);
    lv_label_set_text(status, "请将面部对准摄像头");
    lv_obj_set_style_text_color(status, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_font(status, app_font_scaled(15), 0);

    lv_obj_t * cancel = lv_button_create(root);
    lv_obj_set_size(cancel, SX(140), SY(48));
    lv_obj_add_style(cancel, &st_ghost_btn, 0);
    lv_obj_set_style_radius(cancel, SX(24), 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cancel, SX(8), 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    ui_icon_create(cancel, UI_ICON_CLOSE, SX(20), theme_color(TH_TEXT));
    lv_obj_t * ct = lv_label_create(cancel);
    lv_label_set_text(ct, "取消");
    lv_obj_set_style_text_color(ct, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(ct, app_font_scaled(15), 0);

    /* ---------- 启动扫描动画 ---------- */
    lv_anim_init(&s_beam_anim);
    lv_anim_set_var(&s_beam_anim, s_scan_beam);
    lv_anim_set_exec_cb(&s_beam_anim, beam_anim_xcb);
    lv_anim_set_values(&s_beam_anim, pad, prev_h - pad - beam_h);
    lv_anim_set_time(&s_beam_anim, 1800);
    lv_anim_set_playback_time(&s_beam_anim, 1800);
    lv_anim_set_repeat_count(&s_beam_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_beam_anim);

    return root;
}

/* 扫描动画：上下穿越（y 坐标随时间在 pad 与 prev_h-pad-beam_h 间往返）*/
static void beam_anim_xcb(void * obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/* 返回主页 */
static void back_cb(lv_event_t * e)
{
    (void)e;
    lv_anim_delete(s_scan_beam, NULL);
    ui_switch_page(PAGE_HOME);
}

static void cancel_cb(lv_event_t * e)
{
    back_cb(e);
}
