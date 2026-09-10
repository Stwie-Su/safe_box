/**
 * @file page_face.c
 * 人脸识别全屏页（v5）：
 *   - 顶部：返回按钮 + 标题「人脸识别」
 *   - 中央：实时视频预览面板（自适应大小），**保持 4:3 原始比例居中最大化**
 *          （不拉伸变形），四角扫描框 + 扫描光带 + **双 FPS 角标**（视频 / UI）
 *          叠加在画面之上；无摄像头时显示占位文字
 *   - 底部：状态文字「请将面部对准摄像头」 + 取消按钮
 *
 * 设计变更：
 *   v5（2026-09-10）：
 *   - **保持比例不拉伸**：v4 是把 320×240 拉伸到整个面板（变形）。v5 改为
 *     「等比缩放 + 居中」：scale = min(canvas_w/320, canvas_h/240)（千分比整型
 *     运算，无浮点），得到最大 4:3 矩形，居中放置，四周填面板底色。
 *     满足「图像尽可能大」+「人脸在正中心」+「别拉伸比例」三个要求。
 *   - **双 FPS 分别显示**：
 *       · 视频 fps = face_thread_get_preview() 成功取到的帧数（摄像头真实出帧率）
 *       · UI   fps = LV_EVENT_REFR_READY 事件计数（LVGL 真实渲染帧率）
 *     二者 500ms 滑窗独立统计，右上角徽章两行显示。
 *   - **性能优化（流畅）**：预计算 x/y 映射表（rebuild_maps，SIZE_CHANGED 时
 *     重建一次），绘制期用查表代替逐像素除法——1024×600 面板下每帧省去约
 *     123 万次整型除法，改为 61 万次查表。
 *
 *   v4（2026-09-10）：预览面板自适应 + 画面填满 + 单 FPS 角标。
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
#include "hal/face/face_thread.h"
#include "hal/hal_time.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 采集分辨率：320×240（需求 v1.6 FR-16 锁定，4:3 原始比例） */
#define CAP_W  320
#define CAP_H  240
#define PREVIEW_MS 50      /* 20fps 拉帧周期；YUYV@320x240 实测上限 20fps */

/* FPS 统计滑窗长度 */
#define FPS_WINDOW_MS 500

/* 底部浮层（状态文字 + 取消按钮）高度基准像素；半透明叠在视频最下方 */
#define BOTTOM_BAR_H  48

/* ===== 静态对象句柄 ===== */
static lv_obj_t * s_scan_beam;        // 顶部扫描光带
static lv_obj_t * s_canvas;           // 预览画布（满铺面板，内容居中 4:3）
static lv_obj_t * s_ph_label;         // 占位文字（首帧后隐藏）
static lv_obj_t * s_page_root;        // 页面根容器（可见性判断）
static lv_obj_t * s_preview;          // 预览面板（监听 SIZE_CHANGED）
static lv_obj_t * s_fps_label;        // 双 FPS 角标
static lv_timer_t * s_frame_timer;

/* 画布帧缓冲（动态大小：跟随预览面板，满铺） */
static uint16_t * s_canvas_buf = NULL;
static int32_t    s_canvas_w   = 0;
static int32_t    s_canvas_h   = 0;

/* 视频 4:3 目标矩形（在画布内居中，保持原始比例） */
static int32_t s_vid_x, s_vid_y, s_vid_w, s_vid_h;

/* 最近邻映射表（SIZE_CHANGED 时重建；绘制期查表代替除法） */
static uint16_t * s_map_x = NULL;     /* s_vid_w 个，值域 [0, CAP_W) */
static uint16_t * s_map_y = NULL;     /* s_vid_h 个，值域 [0, CAP_H) */

/* 四角扫描框句柄（用于 SIZE_CHANGED 时重定位）。顺序：左上 h/v / 右上 h/v / 左下 h/v / 右下 h/v */
#define CORNER_OBJ_COUNT 8
static lv_obj_t * s_corners[CORNER_OBJ_COUNT];

/* ---- 双 FPS 计数（各自 500ms 滑窗，独立统计） ---- */
static uint32_t s_fps_win_start;      /* hal_time_ms() 窗口起点（ms） */
static uint32_t s_video_frames;       /* 窗口内 face_thread_get_preview() 成功帧数 */
static uint32_t s_ui_frames;          /* 窗口内 LV_EVENT_REFR_READY 计数 */
static int      s_video_fps;          /* 上次报告的视频 fps */
static int      s_ui_fps;             /* 上次报告的 UI fps */

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
static void compute_video_rect(int32_t cw, int32_t ch);
static void rebuild_maps(void);
static void reposition_corners_and_beam(int32_t w, int32_t h);
static void fps_label_update_if_due(void);
static void ui_refr_ready_cb(lv_event_t * e);

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
    /* 压缩 padding / 行距：把纵向空间尽量让给视频预览（用户要求「图像尽可能大」） */
    lv_obj_set_style_pad_all(root, SX(8), 0);
    lv_obj_set_style_pad_row(root, SY(6), 0);
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
    /* 监听面板大小变化：触发画布/视频矩形/映射表/四角/光带/FPS 重算 */
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

    /* 双 FPS 角标（右上角叠加，半透明圆角徽章，两行） */
    s_fps_label = lv_label_create(s_preview);
    lv_label_set_text(s_fps_label, "视频 — fps\nUI  — fps");
    lv_obj_set_style_text_color(s_fps_label, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_fps_label, app_font_scaled(14), 0);
    lv_obj_set_style_bg_color(s_fps_label, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_fps_label, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_fps_label, SX(10), 0);
    lv_obj_set_style_pad_left(s_fps_label, SX(12), 0);
    lv_obj_set_style_pad_right(s_fps_label, SX(12), 0);
    lv_obj_set_style_pad_top(s_fps_label, SY(5), 0);
    lv_obj_set_style_pad_bottom(s_fps_label, SY(5), 0);
    lv_obj_set_style_border_width(s_fps_label, 0, 0);
    lv_obj_set_style_outline_width(s_fps_label, 0, 0);
    lv_obj_set_style_text_line_space(s_fps_label, SY(2), 0);

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

    /* ---------- 底部浮层：状态文字（左）+ 取消按钮（右） ----------
     * 关键改动（v6）：改为 **preview 的子对象浮在底部**，不再作为 root 的 flex 子项。
     * 原布局下 status/cancel 会挤占纵向空间，导致预览面板只有 ~265px 高、视频被压到
     * 373×280；改为浮层后 preview 独占中间全部空间，视频可达 ~602×452（面积 2.6 倍）。
     * 浮层用半透明底条，不遮挡画面中心（人脸区）。 */
    lv_obj_t * bottom = lv_obj_create(s_preview);
    lv_obj_set_size(bottom, lv_pct(100), SY(BOTTOM_BAR_H));
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_outline_width(bottom, 0, 0);
    lv_obj_set_style_radius(bottom, 0, 0);
    lv_obj_set_style_pad_left(bottom, SX(20), 0);
    lv_obj_set_style_pad_right(bottom, SX(20), 0);
    lv_obj_set_style_pad_top(bottom, 0, 0);
    lv_obj_set_style_pad_bottom(bottom, 0, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * status = lv_label_create(bottom);
    lv_label_set_text(status, "请将面部对准摄像头");
    lv_obj_set_style_text_color(status, theme_color(TH_TEXT), 0);
    lv_obj_set_style_text_font(status, app_font_scaled(15), 0);

    lv_obj_t * cancel = lv_button_create(bottom);
    lv_obj_set_size(cancel, SX(120), SY(40));
    lv_obj_add_style(cancel, &st_ghost_btn, 0);
    lv_obj_set_style_radius(cancel, SX(20), 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cancel, SX(8), 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    ui_icon_create(cancel, UI_ICON_CLOSE, SX(18), theme_color(TH_TEXT));
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

    /* 注册 UI 渲染帧率统计：LV_EVENT_REFR_READY 每次渲染周期结束触发一次 */
    lv_display_add_event_cb(lv_display_get_default(), ui_refr_ready_cb,
                            LV_EVENT_REFR_READY, NULL);

    /* 拉帧定时器（20fps）：回调内部判断页面可见性，隐藏时空转不拉帧 */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
    }

    /* 初始化 FPS 窗口起点 */
    s_fps_win_start = hal_time_ms();
    s_video_frames  = 0;
    s_ui_frames     = 0;
    s_video_fps     = 0;
    s_ui_fps        = 0;

    /* 触发一次 SIZE_CHANGED 立即建 canvas（force layout 后再调用） */
    lv_obj_update_layout(root);
    preview_size_changed_cb(NULL);

    return root;
}

/* ===== UI 渲染帧率：LV_EVENT_REFR_READY 每个渲染周期触发一次 ===== */
static void ui_refr_ready_cb(lv_event_t * e)
{
    (void)e;
    s_ui_frames++;
}

/* ===== SIZE_CHANGED 回调：重建画布 + 视频矩形 + 映射表 + 重定位 ===== */
static void preview_size_changed_cb(lv_event_t * e)
{
    (void)e;
    if (s_preview == NULL) return;
    int32_t w = lv_obj_get_width(s_preview);
    int32_t h = lv_obj_get_height(s_preview);
    if (w <= 0 || h <= 0) return;     /* 初始 0 阶段跳过 */

    rebuild_canvas(w, h);
    compute_video_rect(w, h);
    rebuild_maps();
    reposition_corners_and_beam(w, h);
    /* FPS 角标：右上角，按面板宽度自适配 */
    if (s_fps_label) {
        lv_obj_align(s_fps_label, LV_ALIGN_TOP_RIGHT, -SX(14), SY(12));
    }
}

/* 重建画布缓冲：分配新 buffer 并绑定到 canvas（满铺面板） */
static void rebuild_canvas(int32_t w, int32_t h)
{
    if (w == s_canvas_w && h == s_canvas_h && s_canvas_buf != NULL) return;

    size_t bytes = (size_t)w * (size_t)h * 2u;     /* RGB565 */
    uint16_t * nb = (uint16_t *)malloc(bytes);
    if (nb == NULL) {
        printf("[FACE] canvas 缓冲分配失败 (%u bytes)，保持旧尺寸\n", (unsigned)bytes);
        return;
    }
    /* 初始化为面板底色（视频区外留白用同一底色，视觉上自然） */
    uint16_t bg565 = lv_color_to_u16(theme_color(TH_PANEL));
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) nb[i] = bg565;

    if (s_canvas) {
        lv_canvas_set_buffer(s_canvas, nb, w, h, LV_COLOR_FORMAT_RGB565);
    }
    free(s_canvas_buf);
    s_canvas_buf = nb;
    s_canvas_w   = w;
    s_canvas_h   = h;
}

/* 计算 4:3 视频矩形：等比缩放取最大，居中（不拉伸变形）。
 * scale 用千分比整型运算：scale = min(cw*1000/320, ch*1000/240)。
 * 视频区**吃满整个面板高度**（用户要求「图像尽可能大」）——底部浮层为半透明
 * （OPA 60），叠在视频最下方 ~48px 上，不遮挡画面中心（人脸区）。 */
static void compute_video_rect(int32_t cw, int32_t ch)
{
    int32_t avail_h = ch;                     /* 不避让底部浮层：视频最大化 */

    int32_t sx = (int32_t)(((int64_t)cw * 1000) / CAP_W);
    int32_t sy = (int32_t)(((int64_t)avail_h * 1000) / CAP_H);
    int32_t scale = (sx < sy) ? sx : sy;
    if (scale < 1) scale = 1;

    s_vid_w = (int32_t)(((int64_t)CAP_W * scale) / 1000);
    s_vid_h = (int32_t)(((int64_t)CAP_H * scale) / 1000);
    if (s_vid_w < 1) s_vid_w = 1;
    if (s_vid_h < 1) s_vid_h = 1;
    if (s_vid_w > cw) s_vid_w = cw;
    if (s_vid_h > ch) s_vid_h = ch;
    s_vid_x = (cw - s_vid_w) / 2;             /* 水平居中：人脸/画面在正中心 */
    s_vid_y = (ch - s_vid_h) / 2;             /* 垂直居中 */
    if (s_vid_x < 0) s_vid_x = 0;
    if (s_vid_y < 0) s_vid_y = 0;
}

/* 重建最近邻映射表（查表代替逐像素除法，绘制期零除法） */
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
        printf("[FACE] 映射表分配失败，回退逐像素除法\n");
        return;
    }
    for (int32_t x = 0; x < s_vid_w; x++) {
        int32_t v = (int32_t)(((int64_t)x * CAP_W) / s_vid_w);
        if (v >= CAP_W) v = CAP_W - 1;
        s_map_x[x] = (uint16_t)v;
    }
    for (int32_t y = 0; y < s_vid_h; y++) {
        int32_t v = (int32_t)(((int64_t)y * CAP_H) / s_vid_h);
        if (v >= CAP_H) v = CAP_H - 1;
        s_map_y[y] = (uint16_t)v;
    }
}

/* 把四角扫描框 + 扫描光带重定位到当前**视频区**（s_vid_*，非整个面板）。
 * 因为视频保持 4:3 居中，左右可能有留白；扫描框贴合画面才协调。 */
static void reposition_corners_and_beam(int32_t w, int32_t h)
{
    (void)w; (void)h;
    int32_t pad   = SX(14);
    int32_t len   = SX(30);
    int32_t thick = SY(4);
    int32_t x0 = s_vid_x, y0 = s_vid_y;
    int32_t vw = s_vid_w, vh = s_vid_h;

    /* 左上 */
    lv_obj_set_pos(s_corners[0], x0 + pad, y0 + pad);
    lv_obj_set_size(s_corners[0], len, thick);
    lv_obj_set_pos(s_corners[1], x0 + pad, y0 + pad);
    lv_obj_set_size(s_corners[1], thick, len);
    /* 右上 */
    lv_obj_set_pos(s_corners[2], x0 + vw - pad - len, y0 + pad);
    lv_obj_set_size(s_corners[2], len, thick);
    lv_obj_set_pos(s_corners[3], x0 + vw - pad - thick, y0 + pad);
    lv_obj_set_size(s_corners[3], thick, len);
    /* 左下 */
    lv_obj_set_pos(s_corners[4], x0 + pad, y0 + vh - pad - thick);
    lv_obj_set_size(s_corners[4], len, thick);
    lv_obj_set_pos(s_corners[5], x0 + pad, y0 + vh - pad - len);
    lv_obj_set_size(s_corners[5], thick, len);
    /* 右下 */
    lv_obj_set_pos(s_corners[6], x0 + vw - pad - len, y0 + vh - pad - thick);
    lv_obj_set_size(s_corners[6], len, thick);
    lv_obj_set_pos(s_corners[7], x0 + vw - pad - thick, y0 + vh - pad - len);
    lv_obj_set_size(s_corners[7], thick, len);

    /* 扫描光带：在视频区内左右留 2×pad 余量 */
    int32_t beam_w = vw - 2 * pad;
    int32_t beam_h = SY(3);
    if (beam_w < SY(40)) beam_w = SY(40);
    lv_obj_set_size(s_scan_beam, beam_w, beam_h);
    lv_obj_set_pos(s_scan_beam, x0 + pad, y0 + pad);
    lv_anim_set_values(&s_beam_anim, y0 + pad, y0 + vh - pad - beam_h);
    lv_anim_set_playback_time(&s_beam_anim, 1800);
    lv_anim_set_time(&s_beam_anim, 1800);

    /* 占位文字重新居中（panel 缩放后位置失效） */
    if (s_ph_label) lv_obj_center(s_ph_label);
}

/* ===== 双 FPS 窗口更新：每 500ms 各算一次，更新角标 ===== */
static void fps_label_update_if_due(void)
{
    uint32_t now = hal_time_ms();
    uint32_t elapsed = now - s_fps_win_start;
    if (elapsed >= FPS_WINDOW_MS) {
        if (elapsed > 0) {
            s_video_fps = (int)(((uint64_t)s_video_frames * 1000u) / elapsed);
            s_ui_fps    = (int)(((uint64_t)s_ui_frames    * 1000u) / elapsed);
        }
        s_fps_win_start = now;
        s_video_frames  = 0;
        s_ui_frames     = 0;
        char buf[48];
        snprintf(buf, sizeof(buf), "视频 %d fps\nUI  %d fps", s_video_fps, s_ui_fps);
        lv_label_set_text(s_fps_label, buf);
    }
}

/* ===== 拉帧定时器：拉帧 → 4:3 居中最近邻缩放 → invalidate 视频区 ===== */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_page_root == NULL || lv_obj_is_hidden(s_page_root)) return;
    if (!s_canvas || s_canvas_w <= 0 || s_canvas_h <= 0) return;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    /* 预览帧由 face 线程采集 + 转换（步骤 3b / 规约 §3.4），这里只取最新一帧渲染；
     * 本回调运行在主线程，取回后可直接操作 LVGL，不跨线程。无新帧时保持上一帧。
     * 不再直接调 hal_camera_frame()/release()——相机缓冲归 face 线程所有。 */
    hal_camera_frame_info_t info;
    if (face_thread_get_preview(s_cap_buf, &info)) {
        /* 1. 帧已在 s_cap_buf（face 线程双帧缓冲拷贝而来），无需再拷贝 */

        /* 2. 最近邻缩放 320×240 → 居中 4:3 矩形（查表，绘制期零除法） */
        int32_t dst_stride = s_canvas_w;
        uint16_t bg565 = lv_color_to_u16(theme_color(TH_PANEL));

        /* 2a. 视频区之外的留白填面板底色（仅填上下两条，避免全画布重绘） */
        for (int32_t y = 0; y < s_canvas_h; y++) {
            if (y >= s_vid_y && y < s_vid_y + s_vid_h) continue;
            uint16_t * row = &s_canvas_buf[y * dst_stride];
            for (int32_t x = 0; x < dst_stride; x++) row[x] = bg565;
        }

        /* 2b. 视频行：左右留白 + 中间缩放内容 */
        for (int32_t y = 0; y < s_vid_h; y++) {
            uint16_t * dst_row = &s_canvas_buf[(s_vid_y + y) * dst_stride];
            /* 左侧留白 */
            for (int32_t x = 0; x < s_vid_x; x++) dst_row[x] = bg565;
            /* 中间内容 */
            if (s_map_x && s_map_y) {
                const uint16_t * src_row = &s_cap_buf[(size_t)s_map_y[y] * CAP_W];
                for (int32_t x = 0; x < s_vid_w; x++) {
                    dst_row[s_vid_x + x] = src_row[s_map_x[x]];
                }
            }
            else {
                /* 映射表分配失败时回退：逐像素整型除法 */
                int32_t sy = (int32_t)(((int64_t)y * CAP_H) / s_vid_h);
                const uint16_t * src_row = &s_cap_buf[sy * CAP_W];
                for (int32_t x = 0; x < s_vid_w; x++) {
                    int32_t sx = (int32_t)(((int64_t)x * CAP_W) / s_vid_w);
                    dst_row[s_vid_x + x] = src_row[sx];
                }
            }
            /* 右侧留白 */
            for (int32_t x = s_vid_x + s_vid_w; x < dst_stride; x++) dst_row[x] = bg565;
        }

        /* 3. 统计视频帧（face 线程成功取帧且被主线程消费的计数） */
        s_video_frames++;

        /* 4. 隐藏占位 */
        if (!lv_obj_is_hidden(s_ph_label)) {
            lv_obj_set_hidden(s_ph_label, true);
        }
        /* 只 invalidate 视频矩形（减少重绘面积，提升流畅度） */
        lv_area_t obj_area, a;
        lv_obj_get_coords(s_canvas, &obj_area);
        a.x1 = obj_area.x1 + s_vid_x;
        a.y1 = obj_area.y1 + s_vid_y;
        a.x2 = a.x1 + s_vid_w - 1;
        a.y2 = a.y1 + s_vid_h - 1;
        lv_obj_invalidate_area(s_canvas, &a);
    }
    /* 无新帧（face 线程还没产出 / 无摄像头）时保持上一帧或占位文字。 */

    /* 无论有无新帧，都按窗口推进 FPS 标签（UI fps 需要持续更新） */
    fps_label_update_if_due();
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
