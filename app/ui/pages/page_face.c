/**
 * @file page_face.c
 * 人脸识别全屏页（v4）：
 *   - 顶部：返回按钮 + 标题「人脸识别」
 *   - 中央：实时视频预览面板（自适应大小，**视频填满整个面板**），
 *          四角扫描框 + 扫描光带 + 实时 FPS 角标叠加在画面之上；
 *          无摄像头时显示占位文字
 *   - 底部：状态文字「请将面部对准摄像头」 + 取消按钮
 *
 * 设计变更（v4，2026-09-10）：
 *   - **填满整个视频窗口**：原 v3 预览面板硬编码 360×280，画布 320×240 居中
 *     导致画面在卡片内只占中间一块。v4 让预览面板自适应 root 中间区域
 *     （flex_grow=1），画布动态跟随面板大小（**画面完整填满**，可被拉伸变形
 *     到任意比例），四角扫描框与扫描光带通过 SIZE_CHANGED 事件同步重定位。
 *     PC（1024×600）与开发板（1024×600）走同一套布局，行为一致。
 *   - **实时 FPS**：右上角叠加半透明圆角徽章，500ms 滑窗统计拉到的有效帧数；
 *     单位「fps」取整；拉不到帧时显「— fps」。
 *   - 拉伸算法：**最近邻插值**（性能优先，A7 单核拉满 20fps 不应触发任何浮点）；
 *     源 = hal_camera_frame() 输出的 320×240 RGB565（FR-16 采集锁定），
 *     目标 = 画布当前宽高。
 *
 * 设计约束：
 *   - 摄像头未配置 / 未出帧：显示提示文字；
 *   - 首帧到达后隐藏占位文字；
 *   - 页面隐藏时 timer 空转返回，不拉帧不重绘（页面不销毁，返回后再进自动恢复）；
 *   - 契约（§5.13）：frame() 与 release() 成对调用，漏调会导致后端停更；
 *   - 画布缓冲动态分配：首次创建预览面板后 + 每次 SIZE_CHANGED 时
 *     （free→malloc 原子替换）；最坏 1024×600×2 ≈ 1.2MB。
 */
#include "page_face.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "core/auth/auth_fsm.h"
#include "hal/hal_camera.h"
#include "hal/hal_time.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 采集分辨率：320×240（需求 v1.6 FR-16 锁定，1:1 直通采集） */
#define CAP_W  320
#define CAP_H  240
#define PREVIEW_MS 50      /* 20fps 拉帧周期；YUYV@320x240 实测上限 20fps */

/* FPS 统计滑窗长度 */
#define FPS_WINDOW_MS 500

/* ===== 静态对象句柄 ===== */
static lv_obj_t * s_scan_beam;        // 顶部扫描光带
static lv_obj_t * s_canvas;           // 预览画布（RGB565 拉伸后）
static lv_obj_t * s_ph_label;         // 占位文字（首帧后隐藏）
static lv_obj_t * s_page_root;        // 页面根容器（可见性判断）
static lv_obj_t * s_preview;          // 预览面板（监听 SIZE_CHANGED）
static lv_obj_t * s_fps_label;        // FPS 角标
static lv_timer_t * s_frame_timer;

/* 画布帧缓冲（动态大小：跟随预览面板） */
static uint16_t * s_canvas_buf = NULL;
static int32_t    s_canvas_w   = 0;
static int32_t    s_canvas_h   = 0;

/* 四角扫描框句柄（用于 SIZE_CHANGED 时重定位）。顺序：左上 h/v / 右上 h/v / 左下 h/v / 右下 h/v */
#define CORNER_OBJ_COUNT 8
static lv_obj_t * s_corners[CORNER_OBJ_COUNT];

/* FPS 计数（使用 hal_time_ms() 毫秒单调时钟，避免 hal_time() 秒级精度不足） */
static uint32_t s_fps_window_start;   /* hal_time_ms() 起始时间戳（ms） */
static uint32_t s_fps_frame_count;    /* 窗口内累计拉到的帧数 */
static int      s_fps_last;           /* 上一次报告的 fps（避免无帧时显示 0 抖动） */

/* 采集源临时缓冲（画布是显示缓冲，源是摄像头输出；大小固定 = 320×240×2 = 150KB） */
static uint16_t s_cap_buf[CAP_W * CAP_H];

/* 扫描动画状态 */
static lv_anim_t s_beam_anim;

/* ----- 私有声明 ----- */
static lv_obj_t * mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
                            int32_t w, int32_t h, lv_color_t color);
static void back_cb(lv_event_t * e);
static void cancel_cb(lv_event_t * e);
static void beam_anim_xcb(void * obj, int32_t v);
static void frame_timer_cb(lv_timer_t * t);
static void preview_size_changed_cb(lv_event_t * e);
static void rebuild_canvas(int32_t w, int32_t h);
static void reposition_corners_and_beam(int32_t w, int32_t h);
static void fps_label_update_if_due(void);

/**
 * @brief 在指定位置画一个填充矩形（L 形角的一段），返回对象指针便于 SIZE_CHANGED 重定位
 */
static lv_obj_t * mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
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
    return o;
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

    /* 「← 返回」图标 + 文字按钮 */
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

    /* ---------- 中央：视频预览面板（自适应 root 中间区域，flex grow 占满） ---------- */
    s_preview = lv_obj_create(root);
    /* 100% 宽，flex_grow=1 占满 top/status/cancel 之间的所有可用纵向空间。
     * 不限制纵横比——按用户要求「视频填满整个视频窗口」，可被拉伸变形。
     * 最小尺寸保证极小屏也能看到画面。 */
    lv_obj_set_size(s_preview, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_preview, 1);
    lv_obj_set_style_min_width(s_preview, SY(240), 0);
    lv_obj_set_style_min_height(s_preview, SY(180), 0);
    lv_obj_add_style(s_preview, &st_panel, 0);
    lv_obj_set_style_radius(s_preview, SX(18), 0);
    lv_obj_set_style_bg_color(s_preview, theme_color(TH_PANEL), 0);
    lv_obj_set_style_border_width(s_preview, 0, 0);
    lv_obj_set_style_outline_width(s_preview, 0, 0);
    lv_obj_set_style_pad_all(s_preview, 0, 0);     /* 不要 st_panel 那个 16px pad */
    /* 监听面板大小变化：触发画布重建 + 四角/光带/FPS 重定位 + 占位居中 */
    lv_obj_add_event_cb(s_preview, preview_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    /* 预览画布：尺寸由 SIZE_CHANGED 回调里设置，初始先不绑 buffer */
    s_canvas = lv_canvas_create(s_preview);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);    /* 拉不到帧时透出 panel 底色 */
    lv_canvas_fill_bg(s_canvas, theme_color(TH_PANEL), LV_OPA_COVER);

    /* 占位文字（无摄像头/未出帧时可见；首帧后隐藏） */
    s_ph_label = lv_label_create(s_preview);
    lv_label_set_text(s_ph_label, "视频预览区域\n（摄像头未配置）");
    lv_obj_set_style_text_color(s_ph_label, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_font(s_ph_label, app_font_scaled(15), 0);
    lv_obj_set_style_text_align(s_ph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_ph_label);

    /* FPS 角标（右上角叠加，半透明圆角徽章） */
    s_fps_label = lv_label_create(s_preview);
    lv_label_set_text(s_fps_label, "— fps");
    lv_obj_set_style_text_color(s_fps_label, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_fps_label, app_font_scaled(16), 0);
    lv_obj_set_style_bg_color(s_fps_label, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_fps_label, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_fps_label, SX(10), 0);
    lv_obj_set_style_pad_left(s_fps_label, SX(14), 0);
    lv_obj_set_style_pad_right(s_fps_label, SX(14), 0);
    lv_obj_set_style_pad_top(s_fps_label, SY(6), 0);
    lv_obj_set_style_pad_bottom(s_fps_label, SY(6), 0);
    lv_obj_set_style_border_width(s_fps_label, 0, 0);
    lv_obj_set_style_outline_width(s_fps_label, 0, 0);
    /* FPS 标签创建时还不知道面板大小，初始置到右上角；SIZE_CHANGED 时再校准 */

    /* 4 角扫描框（按 LVGL 创建顺序会叠在 canvas 之上；SIZE_CHANGED 时重定位） */
    int32_t init_w = SY(360);
    int32_t init_h = SY(280);
    int32_t pad = SX(20);
    int32_t len = SX(36);
    int32_t thick = SY(4);

    /* 左上角 */
    s_corners[0] = mk_corner(s_preview, pad, pad, len, thick, theme_color(TH_ACCENT));
    s_corners[1] = mk_corner(s_preview, pad, pad, thick, len, theme_color(TH_ACCENT));
    /* 右上角 */
    s_corners[2] = mk_corner(s_preview, init_w - pad - len, pad, len, thick, theme_color(TH_ACCENT));
    s_corners[3] = mk_corner(s_preview, init_w - pad - thick, pad, thick, len, theme_color(TH_ACCENT));
    /* 左下角 */
    s_corners[4] = mk_corner(s_preview, pad, init_h - pad - thick, len, thick, theme_color(TH_ACCENT));
    s_corners[5] = mk_corner(s_preview, pad, init_h - pad - len, thick, len, theme_color(TH_ACCENT));
    /* 右下角 */
    s_corners[6] = mk_corner(s_preview, init_w - pad - len, init_h - pad - thick, len, thick, theme_color(TH_ACCENT));
    s_corners[7] = mk_corner(s_preview, init_w - pad - thick, init_h - pad - len, thick, len, theme_color(TH_ACCENT));

    /* 扫描光带（按父面板宽度初始放置） */
    s_scan_beam = lv_obj_create(s_preview);
    int32_t beam_w = init_w - 2 * pad - SX(80);
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
    lv_anim_set_values(&s_beam_anim, pad, init_h - pad - beam_h);
    lv_anim_set_time(&s_beam_anim, 1800);
    lv_anim_set_playback_time(&s_beam_anim, 1800);
    lv_anim_set_repeat_count(&s_beam_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_beam_anim);

    /* 拉帧定时器（20fps）：回调内部判断页面可见性，隐藏时空转不拉帧 */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
    }

    /* 初始化 FPS 窗口起点 */
    s_fps_window_start = hal_time_ms();
    s_fps_frame_count  = 0;
    s_fps_last         = 0;

    /* 触发一次 SIZE_CHANGED 立即建 canvas（force layout 后再调用） */
    lv_obj_update_layout(root);
    preview_size_changed_cb(NULL);

    return root;
}

/* ===== SIZE_CHANGED 回调：重建画布 + 重定位四角/光带/FPS 角标 ===== */
static void preview_size_changed_cb(lv_event_t * e)
{
    (void)e;
    if (s_preview == NULL) return;
    int32_t w = lv_obj_get_width(s_preview);
    int32_t h = lv_obj_get_height(s_preview);
    if (w <= 0 || h <= 0) return;     /* 初始 0 阶段跳过 */

    rebuild_canvas(w, h);
    reposition_corners_and_beam(w, h);
    /* FPS 角标：右上角，按面板宽度自适配 */
    if (s_fps_label) {
        lv_obj_align(s_fps_label, LV_ALIGN_TOP_RIGHT, -SX(14), SY(12));
    }
}

/* 重建画布：分配新 buffer，绑定到 canvas。仅在尺寸变化时调用 */
static void rebuild_canvas(int32_t w, int32_t h)
{
    if (w == s_canvas_w && h == s_canvas_h && s_canvas_buf != NULL) return;

    /* 分配新缓冲 */
    size_t bytes = (size_t)w * (size_t)h * 2u;     /* RGB565 */
    uint16_t * nb = (uint16_t *)malloc(bytes);
    if (nb == NULL) {
        printf("[FACE] canvas 缓冲分配失败 (%u bytes)，保持旧尺寸\n", (unsigned)bytes);
        return;
    }
    /* 初始化为面板底色（避免分配瞬间露出原残留） */
    lv_color_t bg = theme_color(TH_PANEL);
    uint16_t bg565 = lv_color_to_u16(bg);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) nb[i] = bg565;

    /* 切到新 buffer：原子替换 */
    if (s_canvas) {
        lv_canvas_set_buffer(s_canvas, nb, w, h, LV_COLOR_FORMAT_RGB565);
    }
    free(s_canvas_buf);
    s_canvas_buf = nb;
    s_canvas_w   = w;
    s_canvas_h   = h;
    /* 占位文字在没拿到帧时一直显示；若之前已隐藏，新尺寸下无帧也会被 canvas 盖住 */
}

/* 把四角扫描框 + 扫描光带重定位到当前面板尺寸 */
static void reposition_corners_and_beam(int32_t w, int32_t h)
{
    int32_t pad   = SX(20);
    int32_t len   = SX(36);
    int32_t thick = SY(4);
    /* 左上 */
    lv_obj_set_pos(s_corners[0], pad, pad);
    lv_obj_set_size(s_corners[0], len, thick);
    lv_obj_set_pos(s_corners[1], pad, pad);
    lv_obj_set_size(s_corners[1], thick, len);
    /* 右上 */
    lv_obj_set_pos(s_corners[2], w - pad - len, pad);
    lv_obj_set_size(s_corners[2], len, thick);
    lv_obj_set_pos(s_corners[3], w - pad - thick, pad);
    lv_obj_set_size(s_corners[3], thick, len);
    /* 左下 */
    lv_obj_set_pos(s_corners[4], pad, h - pad - thick);
    lv_obj_set_size(s_corners[4], len, thick);
    lv_obj_set_pos(s_corners[5], pad, h - pad - len);
    lv_obj_set_size(s_corners[5], thick, len);
    /* 右下 */
    lv_obj_set_pos(s_corners[6], w - pad - len, h - pad - thick);
    lv_obj_set_size(s_corners[6], len, thick);
    lv_obj_set_pos(s_corners[7], w - pad - thick, h - pad - len);
    lv_obj_set_size(s_corners[7], thick, len);

    /* 扫描光带：占宽 = 面板宽 - 2×pad - 80，留两端呼吸余量 */
    int32_t beam_w = w - 2 * pad - SX(80);
    int32_t beam_h = SY(3);
    if (beam_w < SY(40)) beam_w = SY(40);
    lv_obj_set_size(s_scan_beam, beam_w, beam_h);
    lv_obj_set_pos(s_scan_beam, pad + SX(40), pad);
    /* 动画范围同步更新 */
    lv_anim_set_values(&s_beam_anim, pad, h - pad - beam_h);
    /* 强制重置动画进度（lv_anim_set_values 对运行中动画下一轮才生效） */
    lv_anim_set_playback_time(&s_beam_anim, 1800);
    lv_anim_set_time(&s_beam_anim, 1800);

    /* 占位文字重新居中（panel 缩放后位置失效） */
    if (s_ph_label) lv_obj_center(s_ph_label);
}

/* ===== FPS 窗口更新：每 500ms 计算一次滑窗 fps，更新角标 ===== */
static void fps_label_update_if_due(void)
{
    uint32_t now = hal_time_ms();
    uint32_t elapsed = now - s_fps_window_start;
    if (elapsed >= FPS_WINDOW_MS) {
        if (elapsed > 0) {
            /* fps = frames * 1000 / elapsed_ms  → 整数除法 */
            int fps = (int)((uint64_t)s_fps_frame_count * 1000u / elapsed);
            s_fps_last = fps;
        }
        s_fps_window_start = now;
        s_fps_frame_count  = 0;
        char buf[24];
        snprintf(buf, sizeof(buf), "%d fps", s_fps_last);
        lv_label_set_text(s_fps_label, buf);
    }
}

/* ===== 拉帧定时器：拉帧 → 最近邻拉伸 → invalidate ===== */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_page_root == NULL || lv_obj_is_hidden(s_page_root)) return;
    if (!s_canvas || s_canvas_w <= 0 || s_canvas_h <= 0) return;

    const uint8_t * frame = NULL;
    hal_camera_frame_info_t info;
    safe_err_t e = hal_camera_frame(&frame, &info);
    if (e == SAFE_OK && frame != NULL) {
        /* 1. 拷贝 320×240 源到临时缓冲（按 uint16_t 读避免对齐问题） */
        memcpy(s_cap_buf, frame, sizeof(s_cap_buf));

        /* 2. 最近邻拉伸 320×240 → canvas (s_canvas_w × s_canvas_h) */
        int32_t dst_w = s_canvas_w;
        int32_t dst_h = s_canvas_h;
        for (int32_t y = 0; y < dst_h; y++) {
            /* 源 y = y * CAP_H / dst_h ——整型，避免浮点 */
            int32_t sy = (int32_t)((uint32_t)y * CAP_H / (uint32_t)dst_h);
            const uint16_t * src_row = &s_cap_buf[sy * CAP_W];
            uint16_t       * dst_row = &s_canvas_buf[y * dst_w];
            for (int32_t x = 0; x < dst_w; x++) {
                int32_t sx = (int32_t)((uint32_t)x * CAP_W / (uint32_t)dst_w);
                dst_row[x] = src_row[sx];
            }
        }

        hal_camera_release_frame();

        /* 3. 统计 FPS */
        s_fps_frame_count++;
        fps_label_update_if_due();

        /* 4. 隐藏占位 */
        if (!lv_obj_is_hidden(s_ph_label)) {
            lv_obj_set_hidden(s_ph_label, true);
        }
        lv_obj_invalidate(s_canvas);
    }
    else if (e == SAFE_ERR_BUSY) {
        /* 摄像头帧率落后于拉帧周期：保持上一帧，正常现象 */
    }
    else {
        /* 未启动 / 后端不支持：保持占位文字可见 */
        hal_camera_release_frame();
        /* 即便无帧，也按窗口推进 fps 标签——保持「— fps」/「0 fps」语义 */
        fps_label_update_if_due();
    }
}

/* 扫描动画 y 坐标更新 */
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