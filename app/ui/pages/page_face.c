/**
 * @file page_face.c
 * 人脸识别全屏页（v3）：
 *   - 顶部：返回按钮 + 标题「人脸识别」
 *   - 中央：320×240 视频预览（Sprint3 步骤 2：真实摄像头帧，FR-16），
 *          四角扫描框 + 扫描光带叠加在画面之上；无摄像头时显示占位文字
 *   - 底部：状态文字「请将面部对准摄像头」 + 取消按钮
 *
 * 设计约束：
 *   - 摄像头未配置 / 未出帧：显示提示文字「视频预览区域（摄像头未配置）」；
 *   - 首帧到达后隐藏占位文字，之后 50ms（20fps）周期拉帧；
 *   - 页面隐藏时 timer 空转返回，不拉帧不重绘（页面不销毁，返回后再进自动恢复）；
 *   - 契约（§5.13）：frame() 与 release() 成对调用，漏调会导致后端停更。
 */
#include "page_face.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "core/auth/auth_fsm.h"
#include "hal/hal_camera.h"
#include <stdlib.h>
#include <string.h>

/* 预览分辨率：采集 = 显示 = 320×240，1:1 直通（需求 v1.6 FR-16 / 规约 §4.2） */
#define PREVIEW_W  320
#define PREVIEW_H  240
#define PREVIEW_MS 50      /* 20fps 拉帧周期；摄像头 YUYV@320x240 实测上限 20fps */

/* ===== 静态对象句柄 ===== */
static lv_obj_t * s_scan_beam;        // 顶部扫描光带（细长矩形）
static lv_obj_t * s_canvas;           // 预览画布（RGB565 直绘）
static lv_obj_t * s_ph_label;         // 占位文字（首帧后隐藏）
static lv_obj_t * s_page_root;        // 页面根容器（可见性判断）
/* 帧缓冲：固定按 RGB565 字节数分配（uint16_t），与 lv_color_t 深度解耦——
 * 板子构建 LV_COLOR_DEPTH=32 时若用 lv_color_t 数组，sizeof 会翻倍导致 memcpy 越界读。
 * 320×240×2 = 150KB。 */
static uint16_t s_frame_buf[PREVIEW_W * PREVIEW_H];
static lv_timer_t * s_frame_timer;

/* ----- 私有声明 ----- */
static void mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
                      int32_t w, int32_t h, lv_color_t color);
static void back_cb(lv_event_t * e);
static void cancel_cb(lv_event_t * e);
static void beam_anim_xcb(void * obj, int32_t v);
static void frame_timer_cb(lv_timer_t * t);
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
    s_page_root = root;
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

    /* ---------- 中央：视频预览面板（320×240 画面 1:1 居中，四角框叠加在画面上） ---------- */
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

    /* 预览画布：320×240 RGB565，面板内居中（(360-320)/2, (280-240)/2）。
     * 板子构建 LV_COLOR_DEPTH=32 时 LVGL 绘制层自动转换，接口契约仍为 RGB565。 */
    s_canvas = lv_canvas_create(preview);
    lv_canvas_set_buffer(s_canvas, s_frame_buf, PREVIEW_W, PREVIEW_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, (prev_w - PREVIEW_W) / 2, (prev_h - PREVIEW_H) / 2);
    lv_canvas_fill_bg(s_canvas, theme_color(TH_PANEL), LV_OPA_COVER);

    /* 占位文字（无摄像头/未出帧时可见；首帧后隐藏） */
    s_ph_label = lv_label_create(preview);
    lv_label_set_text(s_ph_label, "视频预览区域\n（摄像头未配置）");
    lv_obj_set_style_text_color(s_ph_label, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_font(s_ph_label, app_font_scaled(15), 0);
    lv_obj_set_style_text_align(s_ph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_ph_label);

    /* 4 角扫描框（用 4 个 L 形组合，每个角由两段矩形构成）。
     * 创建顺序在 canvas 之后 → 叠加在画面之上（FR-16「可叠加人脸框与状态提示」）。 */
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

    /* ---------- 启动扫描动画 + 预览拉帧定时器 ---------- */
    lv_anim_init(&s_beam_anim);
    lv_anim_set_var(&s_beam_anim, s_scan_beam);
    lv_anim_set_exec_cb(&s_beam_anim, beam_anim_xcb);
    lv_anim_set_values(&s_beam_anim, pad, prev_h - pad - beam_h);
    lv_anim_set_time(&s_beam_anim, 1800);
    lv_anim_set_playback_time(&s_beam_anim, 1800);
    lv_anim_set_repeat_count(&s_beam_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_beam_anim);

    /* 拉帧定时器（20fps）：回调内部判断页面可见性，隐藏时空转不拉帧 */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
    }

    return root;
}

/* 预览拉帧：hal_camera_frame → memcpy 进画布 → invalidate。
 * 首帧成功后隐藏占位文字；摄像头无帧（BUSY）时保持当前画面不闪烁。 */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_page_root == NULL || lv_obj_is_hidden(s_page_root)) return;
    if (!s_canvas) return;

    const uint8_t * frame = NULL;
    hal_camera_frame_info_t info;
    safe_err_t e = hal_camera_frame(&frame, &info);
    if (e == SAFE_OK && frame != NULL) {
        memcpy(s_frame_buf, frame, (size_t)PREVIEW_W * PREVIEW_H * 2u);   /* RGB565 定长 */
        hal_camera_release_frame();              /* §5.13 契约：用完必须归还 */
        if (!lv_obj_is_hidden(s_ph_label)) {
            lv_obj_set_hidden(s_ph_label, true);  /* 首帧到达，隐藏占位文字 */
        }
        lv_obj_invalidate(s_canvas);
    }
    else if (e == SAFE_ERR_BUSY) {
        /* 摄像头帧率落后于拉帧周期：保持上一帧，正常现象 */
    }
    else {
        /* 未启动 / 后端不支持：保持占位文字可见 */
        hal_camera_release_frame();
    }
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

