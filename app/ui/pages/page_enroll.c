/**
 * @file page_enroll.c
 * 人脸录入页（FR-19/FR-27 细化）：录入专用全屏窗口，与「人脸识别」页分开
 *（用户拍板 2026-09-15：录入只管录入，识别只管识别）。
 *
 * 结构（root 纵向 flex）：
 *   1) 头部单行：标题「人脸录入」+ 引导副文案（模组 NOTE 实时状态）
 *   2) 画面区：视频预览面板（占满）+ 引导小字徽章（叠在视频顶部居中）
 *   3) 底部行：状态文案 + 「取消录入」（回用户页；离开本页即 duty-cycle 终止会话）
 *
 * 生命周期：用户页发起录入 → 切入本页；ENROLL_DONE（成功/失败/30s 兜底超时）
 * 由 page_users 的 on_face_event_ui 统一处理并自动切回用户页。
 *
 * 帧管线与 page_face 同构（拉帧 → 旋转 → 最近邻缩放 → 查表绘制）；两页互斥
 * 可见，face_thread 双帧缓冲任一时刻只有一个消费者。
 * TODO(架构债)：预览管线与 page_face 重复，后续抽 ui/preview 共用组件。
 */
#include "ui/pages/page_enroll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "ui/ui_feedback.h"
#include "ui/ui.h"
#include "hal/hal_face.h"
#include "hal/hal_camera.h"
#include "hal/face/face_thread.h"
#include "hal/hal_time.h"

#define CAP_W  320
#define CAP_H  240
#define PREVIEW_MS 50

/* 画面「陈旧」判定：超过此时长没有新帧即视为无信号 */
#define FACE_FRAME_STALE_MS 2500

/* ===== 静态句柄 ===== */
static lv_obj_t * s_root;
static lv_obj_t * s_sub;              /* 头部引导副文案（模组 NOTE 实时状态） */
static lv_obj_t * s_preview;
static lv_obj_t * s_canvas;
static lv_obj_t * s_guide_lb;         /* 录入实时引导小字（叠在视频顶部居中） */
static lv_obj_t * s_status;           /* 底部状态文案 */
static lv_timer_t * s_frame_timer;
static lv_timer_t * s_status_timer;

/* ===== 帧管线（与 page_face 同构；TODO 抽共用组件） ===== */
static uint16_t * s_canvas_buf;
static int32_t    s_canvas_w, s_canvas_h;
static int32_t    s_vid_x, s_vid_y, s_vid_w, s_vid_h;
static uint16_t * s_map_x, * s_map_y;
static uint16_t   s_cap_buf[CAP_W * CAP_H];
static int        s_rot_deg;
static int32_t    s_scale_max;
static int32_t    s_src_w = CAP_W, s_src_h = CAP_H;
static uint16_t * s_rot_buf;
static bool       s_got_frame;
static uint32_t   s_last_frame_ms;

static void frame_timer_cb(lv_timer_t * t);
static void status_timer_cb(lv_timer_t * t);
static void cancel_cb(lv_event_t * e);
static void preview_env_init(void);
static void rotate_frame(const uint16_t * src, uint16_t * dst,
                         int32_t sw, int32_t sh, int deg);
static void compute_video_rect(int32_t cw, int32_t ch);
static void rebuild_maps(void);
static void rebuild_canvas(void);
static const char * guide_text(int32_t fs);

/* ===== env 开关（与 page_face 共用同一组 env；板上不设即走默认） ===== */
static void preview_env_init(void)
{
    const char * er = getenv("SAFE_CAMERA_ROT");
    if (er != NULL) {
        int v = atoi(er);
        if (v == 90 || v == 180 || v == 270) s_rot_deg = v;
    }
    const char * es = getenv("SAFE_FACE_PREVIEW_SCALE");
    if (es != NULL) {
        int32_t v = (int32_t)atoi(es);
        if (v >= 100 && v <= 5000) s_scale_max = v;
    }
    s_src_w = (s_rot_deg == 90 || s_rot_deg == 270) ? CAP_H : CAP_W;
    s_src_h = (s_rot_deg == 90 || s_rot_deg == 270) ? CAP_W : CAP_H;
    if (s_rot_deg != 0 && s_rot_buf == NULL) {
        s_rot_buf = (uint16_t *)malloc(sizeof(uint16_t) * CAP_W * CAP_H);
        if (s_rot_buf == NULL) {
            s_rot_deg = 0;
            s_src_w   = CAP_W;
            s_src_h   = CAP_H;
        }
    }
}

/* 把 sw×sh 的 RGB565 帧旋转 deg 后写入 dst（90 = 顺时针） */
static void rotate_frame(const uint16_t * src, uint16_t * dst, int32_t sw, int32_t sh, int deg)
{
    if (deg == 90) {
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)x * sh + (sh - 1 - y)] = src[(size_t)y * sw + x];
    } else if (deg == 180) {
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)(sh - 1 - y) * sw + (sw - 1 - x)] = src[(size_t)y * sw + x];
    } else {   /* 270 */
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)(sw - 1 - x) * sh + y] = src[(size_t)y * sw + x];
    }
}

/* 计算 4:3（或旋转后 3:4）视频矩形：等比缩放取最大，居中不拉伸 */
static void compute_video_rect(int32_t cw, int32_t ch)
{
    int32_t sx = (int32_t)(((int64_t)cw * 1000) / s_src_w);
    int32_t sy = (int32_t)(((int64_t)ch * 1000) / s_src_h);
    int32_t scale = (sx < sy) ? sx : sy;
    if (scale < 1) scale = 1;
    /* 放大封顶按 UI 缩放系数同步放大（上限语义 = 设计空间 1024×600 的倍数） */
    if (scale > (int32_t)(s_scale_max * ui_scale_x()))
        scale = (int32_t)(s_scale_max * ui_scale_x());

    s_vid_w = (int32_t)(((int64_t)s_src_w * scale) / 1000);
    s_vid_h = (int32_t)(((int64_t)s_src_h * scale) / 1000);
    if (s_vid_w < 1) s_vid_w = 1;
    if (s_vid_h < 1) s_vid_h = 1;
    if (s_vid_w > cw) s_vid_w = cw;
    if (s_vid_h > ch) s_vid_h = ch;
    s_vid_x = (cw - s_vid_w) / 2;
    s_vid_y = (ch - s_vid_h) / 2;
    if (s_vid_x < 0) s_vid_x = 0;
    if (s_vid_y < 0) s_vid_y = 0;
}

static void rebuild_maps(void)
{
    free(s_map_x);
    free(s_map_y);
    s_map_x = NULL;
    s_map_y = NULL;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    s_map_x = (uint16_t *)malloc((size_t)s_vid_w * sizeof(uint16_t));
    s_map_y = (uint16_t *)malloc((size_t)s_vid_h * sizeof(uint16_t));
    if (s_map_x == NULL || s_map_y == NULL) {
        free(s_map_x); free(s_map_y);
        s_map_x = NULL; s_map_y = NULL;
        return;
    }
    for (int32_t x = 0; x < s_vid_w; x++) {
        int32_t v = (int32_t)(((int64_t)x * s_src_w) / s_vid_w);
        if (v >= s_src_w) v = s_src_w - 1;
        s_map_x[x] = (uint16_t)v;
    }
    for (int32_t y = 0; y < s_vid_h; y++) {
        int32_t v = (int32_t)(((int64_t)y * s_src_h) / s_vid_h);
        if (v >= s_src_h) v = s_src_h - 1;
        s_map_y[y] = (uint16_t)v;
    }
}

/* 画布缓冲：尺寸 = 视频矩形（同 page_face 的 T3 结论，留白零开销） */
static void rebuild_canvas(void)
{
    int32_t cw = s_vid_w;
    int32_t ch = s_vid_h;
    if (cw <= 0 || ch <= 0) return;

    if (cw == s_canvas_w && ch == s_canvas_h && s_canvas_buf != NULL) {
        if (s_canvas) {
            lv_obj_set_size(s_canvas, cw, ch);
            lv_obj_set_pos(s_canvas, s_vid_x, s_vid_y);
        }
        return;
    }

    size_t bytes = (size_t)cw * (size_t)ch * 2u;
    uint16_t * nb = (uint16_t *)malloc(bytes);
    if (nb == NULL) return;
    uint16_t bg565 = lv_color_to_u16(theme_color(TH_PANEL));
    for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++) nb[i] = bg565;

    if (s_canvas) {
        lv_canvas_set_buffer(s_canvas, nb, cw, ch, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_canvas, cw, ch);
        lv_obj_set_pos(s_canvas, s_vid_x, s_vid_y);
    }
    free(s_canvas_buf);
    s_canvas_buf = nb;
    s_canvas_w   = cw;
    s_canvas_h   = ch;
}

/* NOTE state → 引导文案（FR-19；语义见 hal_face.h face_service_face_state） */
static const char * guide_text(int32_t fs)
{
    switch (fs) {
        case 0:  return "已检测到人脸，请保持不动";
        case 2:  return "人脸太靠上，请下移一点";
        case 3:  return "人脸太靠下，请上移一点";
        case 4:  return "人脸太靠左，请右移一点";
        case 5:  return "人脸太靠右，请左移一点";
        default: return "未检测到人脸，请正对模组";
    }
}

/* ===== 拉帧定时器（本页可见才拉帧；两页互斥 → 单消费者） ===== */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_root == NULL || lv_obj_is_hidden(s_root)) return;
    if (!s_canvas || s_canvas_w <= 0 || s_canvas_h <= 0) return;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    hal_camera_frame_info_t info;
    if (face_thread_get_preview(s_cap_buf, &info)) {
        const uint16_t * src = s_cap_buf;
        if (s_rot_deg != 0 && s_rot_buf != NULL) {
            rotate_frame(s_cap_buf, s_rot_buf, CAP_W, CAP_H, s_rot_deg);
            src = s_rot_buf;
        }

        int32_t dst_stride = s_canvas_w;
        uint16_t * buf = s_canvas_buf;
        for (int32_t y = 0; y < s_canvas_h; y++) {
            uint16_t * dst_row = &buf[(size_t)y * dst_stride];
            if (s_map_x && s_map_y) {
                const uint16_t * src_row = &src[(size_t)s_map_y[y] * s_src_w];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    dst_row[x] = src_row[s_map_x[x]];
                }
            } else {
                int32_t sy = (int32_t)(((int64_t)y * s_src_h) / s_canvas_h);
                const uint16_t * src_row = &src[sy * s_src_w];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    int32_t sx = (int32_t)(((int64_t)x * s_src_w) / s_canvas_w);
                    dst_row[x] = src_row[sx];
                }
            }
        }

        s_got_frame = true;
        s_last_frame_ms = hal_time_ms();
        lv_obj_invalidate(s_canvas);
    }
}

/* ===== 状态定时器（500ms）：引导小字 + 状态文案 ===== */
static void status_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_root == NULL || lv_obj_is_hidden(s_root)) return;

    uint32_t now = hal_time_ms();
    bool live = s_got_frame && ((now - s_last_frame_ms) < FACE_FRAME_STALE_MS);
    bool enrolling = face_service_enrolling();

    /* 引导小字：仅录入会话期间显示在视频上（FR-19） */
    if (enrolling) {
        int32_t fs = face_service_face_state();
        lv_label_set_text(s_guide_lb, guide_text(fs));
        lv_obj_set_hidden(s_guide_lb, false);
    } else {
        lv_obj_set_hidden(s_guide_lb, true);
    }

    /* 头部副文案（冗余一份引导，低视觉权重） */
    lv_label_set_text(s_sub, enrolling ? guide_text(face_service_face_state())
                                       : "未在录入会话");

    /* 底部状态文案：明确、不空 */
    const char * st;
    if (!face_service_enrolling())     st = "未在录入会话 · 从「用户」页发起录入";
    else if (!live)                    st = "等待摄像头出帧…";
    else                               st = "录入会话进行中 · 完成后自动返回用户页";
    lv_label_set_text(s_status, st);
}

static void cancel_cb(lv_event_t * e)
{
    (void)e;
    /* 不需要显式 abort：离开本页 → duty-cycle 判定前台离开 → 0x10 RESET 终止在途会话 */
    ui_switch_page(PAGE_USERS);
}

/* ===== SIZE_CHANGED：重算视频矩形 / 映射表 / 画布 / 引导小字位置 ===== */
static void preview_size_changed_cb(lv_event_t * e)
{
    (void)e;
    if (s_preview == NULL) return;
    int32_t w = lv_obj_get_width(s_preview);
    int32_t h = lv_obj_get_height(s_preview);
    if (w <= 0 || h <= 0) return;

    compute_video_rect(w, h);
    rebuild_maps();
    rebuild_canvas();
    if (s_guide_lb) {
        lv_obj_align(s_guide_lb, LV_ALIGN_TOP_MID, 0, s_vid_y + SY(8));
    }
}

lv_obj_t * page_enroll_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    s_root = root;
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, SX(8), 0);
    lv_obj_set_style_pad_row(root, SY(4), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(root, false);

    /* ---------- 头部单行：标题 + 实时引导副文案 ---------- */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_outline_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, SX(12), 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "人脸录入");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(18), 0);

    s_sub = lv_label_create(head);
    lv_label_set_text(s_sub, "");
    lv_obj_add_style(s_sub, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_sub, app_font_scaled(12), 0);
    lv_obj_set_flex_grow(s_sub, 1);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_DOT);

    /* ---------- 画面区：视频预览面板（flex grow 占满） ---------- */
    s_preview = lv_obj_create(root);
    lv_obj_set_size(s_preview, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_preview, 1);
    lv_obj_set_style_min_width(s_preview, SY(240), 0);
    lv_obj_set_style_min_height(s_preview, SY(180), 0);
    lv_obj_add_style(s_preview, &st_panel, 0);
    lv_obj_set_style_radius(s_preview, SX(16), 0);
    lv_obj_set_style_bg_color(s_preview, theme_color(TH_PANEL), 0);
    lv_obj_set_style_border_width(s_preview, 1, 0);
    lv_obj_set_style_border_color(s_preview, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(s_preview, 0, 0);
    lv_obj_set_style_pad_all(s_preview, 0, 0);
    lv_obj_set_scroll_dir(s_preview, LV_DIR_NONE);
    lv_obj_set_scrollable(s_preview, false);
    lv_obj_add_event_cb(s_preview, preview_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    s_canvas = lv_canvas_create(s_preview);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);
    lv_canvas_fill_bg(s_canvas, theme_color(TH_PANEL), LV_OPA_COVER);

    /* 引导小字（叠在视频顶部居中；位置在 SIZE_CHANGED 里随视频矩形更新） */
    s_guide_lb = lv_label_create(s_preview);
    lv_label_set_text(s_guide_lb, "");
    lv_obj_set_style_bg_color(s_guide_lb, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_guide_lb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_guide_lb, SX(8), 0);
    lv_obj_set_style_pad_all(s_guide_lb, SX(8), 0);
    lv_obj_set_style_text_font(s_guide_lb, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(s_guide_lb, theme_color(TH_TEXT), 0);
    lv_obj_set_hidden(s_guide_lb, true);

    /* ---------- 底部行：状态 + 取消 ---------- */
    lv_obj_t * foot = lv_obj_create(root);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_outline_width(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_set_style_pad_column(foot, SX(12), 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(foot, false);

    s_status = lv_label_create(foot);
    lv_label_set_text(s_status, "");
    lv_obj_add_style(s_status, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_status, app_font_scaled(12), 0);
    lv_obj_set_flex_grow(s_status, 1);

    lv_obj_t * cancel = lv_button_create(foot);
    lv_obj_set_size(cancel, SX(128), SY(38));
    lv_obj_set_style_radius(cancel, SX(12), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(cancel, 0, 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cancel, SX(8), 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消录入");
    lv_obj_set_style_text_font(cl, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(cl, theme_color(TH_TEXT), 0);
    lv_obj_center(cl);

    /* ---------- 定时器 ---------- */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
        s_status_timer = lv_timer_create(status_timer_cb, 500, NULL);
    }

    preview_env_init();
    lv_obj_update_layout(root);
    preview_size_changed_cb(NULL);

    return root;
}

bool page_enroll_is_visible(void)
{
    return (s_root != NULL) && !lv_obj_is_hidden(s_root);
}
