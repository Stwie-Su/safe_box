/**
 * @file page_face.c
 * 人脸识别页（v2 卡片化；视觉真源 ui_redesign_preview_v2.html 的 #sc-face）。
 *
 * 布局（root 纵向 flex）：
 *   1) 标题区（.hd）：标题「人脸识别」+ 副标题（后端名 · 模板 n/max · 健康状态，真实来源）
 *   2) 画面区：**视频矩形**（不是满铺内容区）——画布尺寸 = 4:3 视频矩形，显示放大
 *      上限 1.3×（FACE_MAX_SCALE）。这是 T3 帧率优化的既有结论：满铺时每帧给留白
 *      刷底色会吃掉约 3.7× 帧率，v6 已把画布收缩到视频矩形，**本版不得回归**。
 *      · 四角扫描框 + 扫描光带：四态染色（青=待识别 / 绿=已匹配 / 红=未匹配或活体失败），
 *        颜色走 theme_color(TH_ACCENT / TH_OK / TH_DANGER)；
 *      · 右上角两个 FPS 角标（UI fps / 视频 fps），取实测值；
 *      · 无摄像头 / 后端降级：画面区显示「图标 + 一句话」占位，不留黑块。
 *   3) 底部行：左 = 状态文案（后端名 / 健康 / 超时等明确文案，不留空、不显示 "--"）；
 *      右 = 「录入人脸」主按钮 + 「删除模板」次按钮。
 *
 * 安全性说明（有意偏离预览）：
 *   人脸录入 / 删除在既有实现（page_users.c）里是**管理员二次验证（admin PIN）**门禁
 *   的敏感操作，且必须绑定到“某个具体用户”。本页没有用户选择器、也没有可复用的
 *   二次验证弹窗（auth_show_ctx 是 page_users.c 私有），因此这两个按钮**不在本页直接
 *   执行破坏性操作**，而是导航到「用户」页并给出横幅指引，让操作走已验证的安全路径。
 *   后端不支持该能力时按钮置灰 + 状态文案说明（不留空）。
 *
 * 性能与纪律：
 *   - 画布缓冲尺寸 = 视频矩形（非整面板），映射表 SIZE_CHANGED 时重建一次，
 *     绘制期查表代替除法（见 v5/v6 注释，未改动）；
 *   - 页面隐藏时拉帧 timer 空转返回，不拉帧不重绘；
 *   - 契约（§5.13）：frame()/release() 成对；预览帧归 face 线程所有，主线程只取最新帧；
 *   - 颜色 100% 走 theme_color(TH_*) / st_* 样式，零硬编码 hex。
 */
#include "page_face.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/ui_anim.h"
#include "ui/icons.h"
#include "ui/ui_feedback.h"
#include "core/auth/auth_fsm.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "hal/hal_camera.h"
#include "hal/hal_face.h"
#include "hal/face/face_thread.h"
#include "hal/hal_time.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 采集分辨率：320×240（需求 v1.6 FR-16 锁定，4:3 原始比例） */
#define CAP_W  320
#define CAP_H  240
#define PREVIEW_MS 50      /* 20fps 拉帧周期；YUYV@320x240 实测上限 20fps */

/* 显示放大倍数上限（千分比，1300 = 1.3×）。
 * 为什么封顶：LVGL 的绘制耗时与「显示像素数」近似成正比（板上实测 223k 像素 →
 * 渲染周期 121ms ≈ 8fps；130k 像素 → 23ms ≈ 21.6fps）。而采集源固定 320×240，
 * 放大 1.3× 之后每个源像素仍被显示（1.3 个显示像素/源像素），继续放大只是最近邻
 * 复制，不增加任何细节，却线性地吃掉帧率。取 1.3× 是「画面够大」与「UI ≥20fps」
 * 的实测平衡点；面板比 416×312 还小时按面板自适应（取 min），不会过度放大。 */
#define FACE_MAX_SCALE 1300

/* FPS 统计滑窗长度 */
#define FPS_WINDOW_MS 500

/* 画面「陈旧」判定：超过此时长没有新帧即视为无信号（占位重新显示） */
#define FACE_FRAME_STALE_MS 2500

/* 识别结果「保鲜」秒数：超时的旧结果不再驱动状态文案 */
#define FACE_RESULT_FRESH_S 8

/* 状态刷新 / 模板统计周期 */
#define FACE_STATUS_MS 500
#define FACE_USERS_MS  2000

/* ===== 静态对象句柄 ===== */
static lv_obj_t * s_scan_beam;        // 顶部扫描光带
static lv_obj_t * s_canvas;           // 预览画布（尺寸 = 视频矩形）
static lv_obj_t * s_ph;               // 占位容器（图标 + 一句话；首帧后隐藏）
static lv_obj_t * s_ph_icon;          // 占位图标
static lv_obj_t * s_ph_label;         // 占位文字
static lv_obj_t * s_page_root;        // 页面根容器（可见性判断）
static lv_obj_t * s_preview;          // 预览面板（监听 SIZE_CHANGED）
static lv_obj_t * s_fps_box;          // FPS 角标容器（右上角，两个徽章）
static lv_obj_t * s_fps_ui;           // UI fps 徽章
static lv_obj_t * s_fps_vid;          // 视频 fps 徽章
static lv_obj_t * s_face_sub;         // 标题副文案（后端/模板/健康）
static lv_obj_t * s_face_status;      // 底部左侧状态文案
static lv_obj_t * s_btn_enroll;       // 录入人脸（主）
static lv_obj_t * s_btn_del;          // 删除模板（次）
static lv_obj_t * s_btn_enroll_ic;
static lv_obj_t * s_btn_enroll_lb;
static lv_obj_t * s_btn_del_ic;
static lv_obj_t * s_btn_del_lb;
static lv_timer_t * s_frame_timer;

/* 画布帧缓冲：尺寸 = **视频矩形**（不是整个面板）。理由见 rebuild_canvas()。 */
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

/* ---- 扫描框状态染色（UI 现代化 spec §3 后半） ----
 * 状态 → 主题色角色查表；FAIL 为瞬时反馈，UI_ANIM_SCAN_FAIL_MS 后自动回 IDLE。 */
static page_face_scan_t s_scan_state = PAGE_FACE_SCAN_IDLE;
static lv_timer_t * s_scan_revert_timer = NULL;

static const theme_role_t SCAN_ROLE[PAGE_FACE_SCAN_COUNT] = {
    TH_ACCENT,   /* PAGE_FACE_SCAN_IDLE */
    TH_OK,       /* PAGE_FACE_SCAN_OK   */
    TH_DANGER,   /* PAGE_FACE_SCAN_FAIL */
};

/* ---- 双 FPS 计数（各自 500ms 滑窗，独立统计） ---- */
static uint32_t s_fps_win_start;      /* hal_time_ms() 窗口起点（ms） */
static uint32_t s_video_frames;       /* 窗口内 face_thread_get_preview() 成功帧数 */
static uint32_t s_ui_frames;          /* 窗口内 LV_EVENT_REFR_READY 计数 */
static int      s_video_fps;          /* 上次报告的视频 fps */
static int      s_ui_fps;             /* 上次报告的 UI fps */

/* ---- 预览帧新鲜度 / 模板统计 ---- */
static bool     s_got_frame = false;      /* 是否已经出过帧 */
static uint32_t s_last_frame_ms = 0;      /* 最近一次成功取帧时间（hal_time_ms） */
static int      s_tpl_count = 0;          /* 已绑定人脸模板数（= face_id>=0 的用户数） */
static bool     s_users_busy = false;     /* astore_load_users 在途标记 */

/* 采集源临时缓冲（画布是显示缓冲，源是摄像头输出；大小固定 = 320×240×2 = 150KB） */
static uint16_t s_cap_buf[CAP_W * CAP_H];

/* 扫描动画状态 */
static lv_anim_t s_beam_anim;

/* ----- 私有声明 ----- */
static lv_obj_t * mk_corner(lv_obj_t * parent, int32_t x, int32_t y,
                            int32_t w, int32_t h, lv_color_t color);
static lv_obj_t * mk_face_btn(lv_obj_t * parent, ui_glyph_t g, const char * text,
                              bool primary, lv_event_cb_t cb,
                              lv_obj_t ** out_icon, lv_obj_t ** out_label);
static void beam_anim_xcb(void * obj, int32_t v);
static void frame_timer_cb(lv_timer_t * t);
static void preview_size_changed_cb(lv_event_t * e);
static void rebuild_canvas(int32_t w, int32_t h);
static void compute_video_rect(int32_t cw, int32_t ch);
static void rebuild_maps(void);
static void reposition_corners_and_beam(int32_t w, int32_t h);
static void fps_label_update_if_due(void);
static void ui_refr_ready_cb(lv_event_t * e);
/* 扫描框状态染色（spec §3 后半） */
static void scan_apply_color(void);
static void scan_revert_timer_cb(lv_timer_t * t);
static void scan_arm_revert(uint32_t ms);
static void scan_refresh_theme(int idx);
/* v2 新增：标题 / 状态 / 占位 / 按钮 */
static void face_update_header(void);
static void face_status_timer_cb(lv_timer_t * t);
static void face_users_timer_cb(lv_timer_t * t);
static void face_users_loaded(safe_user_t * list, int count);
static void face_refresh_local_colors(void);
static void face_ph_show(bool show, const char * text);
static const char * face_health_str(void);
static const char * face_status_from_result(void);
static void face_set_btn_enabled(lv_obj_t * btn, bool enabled);
static void enroll_cb(lv_event_t * e);
static void delete_tpl_cb(lv_event_t * e);

/* ================================================================
 *  扫描框状态染色（UI 现代化 spec §3 后半）
 *  四角扫描框（8 段）+ 扫描光带统一按当前状态取主题色。
 * ================================================================ */

/* 按当前状态把颜色刷到 8 个角 + 光带 */
static void scan_apply_color(void)
{
    lv_color_t c = theme_color(SCAN_ROLE[s_scan_state]);
    for (int i = 0; i < CORNER_OBJ_COUNT; i++) {
        if (s_corners[i]) lv_obj_set_style_bg_color(s_corners[i], c, 0);
    }
    if (s_scan_beam) lv_obj_set_style_bg_color(s_scan_beam, c, 0);
}

/* FAIL 态自动回 IDLE（一次性 timer 到点） */
static void scan_revert_timer_cb(lv_timer_t * t)
{
    (void)t;
    s_scan_revert_timer = NULL;
    if (s_scan_state == PAGE_FACE_SCAN_FAIL) {
        s_scan_state = PAGE_FACE_SCAN_IDLE;
        scan_apply_color();
    }
}

/* 装填/撤销自动回弹 timer。ms==0 表示不回弹（OK/WARN/IDLE）。 */
static void scan_arm_revert(uint32_t ms)
{
    if (s_scan_revert_timer != NULL) {
        lv_timer_delete(s_scan_revert_timer);
        s_scan_revert_timer = NULL;
    }
    if (ms == 0) return;
    s_scan_revert_timer = lv_timer_create(scan_revert_timer_cb, ms, NULL);
    if (s_scan_revert_timer != NULL) {
        lv_timer_set_repeat_count(s_scan_revert_timer, 1);
    }
}

/* 主题切换：按当前状态重新取色 + 刷新本页新增的本地颜色覆盖 */
static void scan_refresh_theme(int idx)
{
    (void)idx;
    scan_apply_color();
    face_refresh_local_colors();
}

void page_face_set_scan_state(page_face_scan_t st)
{
    if (st >= PAGE_FACE_SCAN_COUNT) return;
    s_scan_state = st;
    scan_apply_color();
    /* 只有 FAIL 需要自动回弹：它是「刚才那次没过」的瞬时反馈，
     * 不能让红色一直挂在扫描框上误导用户（spec §3：FAIL 态 600ms 后回 IDLE）。 */
    scan_arm_revert((st == PAGE_FACE_SCAN_FAIL) ? UI_ANIM_SCAN_FAIL_MS : 0);
}

page_face_scan_t page_face_scan_state(void)
{
    return s_scan_state;
}

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
    lv_obj_set_scrollable(o, false);
    return o;
}

/**
 * @brief 底部操作按钮：primary = accent 实心，否则 ghost 描边（含图标 + 文本）
 */
static lv_obj_t * mk_face_btn(lv_obj_t * parent, ui_glyph_t g, const char * text,
                              bool primary, lv_event_cb_t cb,
                              lv_obj_t ** out_icon, lv_obj_t ** out_label)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_size(btn, SX(primary ? 150 : 132), SY(primary ? 44 : 38));
    lv_obj_set_style_radius(btn, SX(12), 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    if (primary) {
        lv_obj_set_style_bg_color(btn, theme_color(TH_ACCENT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_add_style(btn, &st_ghost_btn, 0);
        lv_obj_set_style_radius(btn, SX(12), 0);
        lv_obj_set_style_border_color(btn, theme_color(TH_BORDER), 0);
    }
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, SX(8), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    *out_icon = icon_label_colored(btn, g, primary ? 19 : 16,
                                   primary ? TH_ACCENT_INK : TH_TEXT);
    *out_label = lv_label_create(btn);
    lv_label_set_text(*out_label, text);
    lv_obj_set_style_text_font(*out_label, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(*out_label, theme_color(primary ? TH_ACCENT_INK : TH_TEXT), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
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
    lv_obj_set_style_pad_all(root, SX(8), 0);
    lv_obj_set_style_pad_row(root, SY(6), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    /* ---------- 标题区（.hd）：标题 + 副文案 ---------- */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_outline_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_left(head, SX(14), 0);
    lv_obj_set_style_pad_right(head, SX(14), 0);
    lv_obj_set_style_pad_row(head, SY(2), 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "人脸识别");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(20), 0);

    s_face_sub = lv_label_create(head);
    lv_label_set_text(s_face_sub, "");   /* 由 face_update_header() 填真实值 */
    lv_obj_add_style(s_face_sub, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_face_sub, app_font_scaled(12), 0);

    /* ---------- 画面区：视频预览面板（flex grow 占满中间） ---------- */
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
    lv_obj_set_style_pad_all(s_preview, 0, 0);     /* 不要 st_panel 那个 16px pad */
    lv_obj_set_scroll_dir(s_preview, LV_DIR_NONE);
    lv_obj_set_scrollable(s_preview, false);
    /* 监听面板大小变化：触发画布/视频矩形/映射表/四角/光带/FPS 重算 */
    lv_obj_add_event_cb(s_preview, preview_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    /* 预览画布：尺寸由 SIZE_CHANGED 回调里设置，初始先不绑 buffer */
    s_canvas = lv_canvas_create(s_preview);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);    /* 拉不到帧时透出 panel 底色 */
    lv_canvas_fill_bg(s_canvas, theme_color(TH_PANEL), LV_OPA_COVER);

    /* 占位（无摄像头 / 无信号时可见）：图标 + 一句话 */
    s_ph = lv_obj_create(s_preview);
    lv_obj_set_size(s_ph, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ph, 0, 0);
    lv_obj_set_style_outline_width(s_ph, 0, 0);
    lv_obj_set_style_pad_all(s_ph, 0, 0);
    lv_obj_set_style_pad_row(s_ph, SY(8), 0);
    lv_obj_set_flex_flow(s_ph, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ph, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_ph, false);

    s_ph_icon = icon_label_colored(s_ph, UI_GLYPH_CAMERA, 44, TH_TEXT_MUT);
    s_ph_label = lv_label_create(s_ph);
    lv_label_set_text(s_ph_label, "摄像头未接入，无预览画面");
    lv_obj_set_style_text_color(s_ph_label, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_font(s_ph_label, app_font_scaled(14), 0);
    lv_obj_set_style_text_align(s_ph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_ph);

    /* 双 FPS 角标（右上角，两个独立徽章：UI fps / 视频 fps） */
    s_fps_box = lv_obj_create(s_preview);
    lv_obj_set_size(s_fps_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_fps_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fps_box, 0, 0);
    lv_obj_set_style_outline_width(s_fps_box, 0, 0);
    lv_obj_set_style_pad_all(s_fps_box, 0, 0);
    lv_obj_set_style_pad_column(s_fps_box, SX(6), 0);
    lv_obj_set_flex_flow(s_fps_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fps_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_fps_box, false);

    lv_obj_t ** fps_tgt[2] = { &s_fps_ui, &s_fps_vid };
    const char * fps_txt[2] = { "UI — fps", "视频 — fps" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t * b = lv_label_create(s_fps_box);
        lv_label_set_text(b, fps_txt[i]);
        lv_obj_set_style_text_color(b, theme_color(TH_TEXT), 0);
        lv_obj_set_style_text_font(b, app_font_scaled(12), 0);
        lv_obj_set_style_bg_color(b, theme_color(TH_PANEL2), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_radius(b, SX(8), 0);
        lv_obj_set_style_pad_left(b, SX(10), 0);
        lv_obj_set_style_pad_right(b, SX(10), 0);
        lv_obj_set_style_pad_top(b, SY(4), 0);
        lv_obj_set_style_pad_bottom(b, SY(4), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_outline_width(b, 0, 0);
        *fps_tgt[i] = b;
    }

    /* 4 角扫描框（按 LVGL 创建顺序会叠在 canvas 之上；SIZE_CHANGED 时重定位） */
    int32_t init_w = SY(360);
    int32_t init_h = SY(280);
    int32_t pad = SX(20);
    int32_t len = SX(36);
    int32_t thick = SY(4);

    s_corners[0] = mk_corner(s_preview, pad, pad, len, thick, theme_color(TH_ACCENT));
    s_corners[1] = mk_corner(s_preview, pad, pad, thick, len, theme_color(TH_ACCENT));
    s_corners[2] = mk_corner(s_preview, init_w - pad - len, pad, len, thick, theme_color(TH_ACCENT));
    s_corners[3] = mk_corner(s_preview, init_w - pad - thick, pad, thick, len, theme_color(TH_ACCENT));
    s_corners[4] = mk_corner(s_preview, pad, init_h - pad - thick, len, thick, theme_color(TH_ACCENT));
    s_corners[5] = mk_corner(s_preview, pad, init_h - pad - len, thick, len, theme_color(TH_ACCENT));
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
    lv_obj_set_scrollable(s_scan_beam, false);

    /* ---------- 底部行：状态文案（左）+ 录入/删除按钮（右） ---------- */
    lv_obj_t * foot = lv_obj_create(root);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_outline_width(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_set_style_pad_left(foot, SX(14), 0);
    lv_obj_set_style_pad_right(foot, SX(14), 0);
    lv_obj_set_style_pad_column(foot, SX(12), 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(foot, false);

    s_face_status = lv_label_create(foot);
    lv_label_set_text(s_face_status, "");   /* 由 face_status_timer_cb 填真实文案 */
    lv_obj_add_style(s_face_status, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_face_status, app_font_scaled(13), 0);
    lv_obj_set_flex_grow(s_face_status, 1);

    lv_obj_t * acts = lv_obj_create(foot);
    lv_obj_set_size(acts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(acts, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(acts, 0, 0);
    lv_obj_set_style_outline_width(acts, 0, 0);
    lv_obj_set_style_pad_all(acts, 0, 0);
    lv_obj_set_style_pad_column(acts, SX(10), 0);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(acts, false);

    s_btn_enroll = mk_face_btn(acts, UI_GLYPH_FACE, "录入人脸", true, enroll_cb,
                               &s_btn_enroll_ic, &s_btn_enroll_lb);
    s_btn_del    = mk_face_btn(acts, UI_GLYPH_DELETE, "删除模板", false, delete_tpl_cb,
                               &s_btn_del_ic, &s_btn_del_lb);

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

    /* 扫描框染色随主题切换重刷（第 4 个主题回调，槽位已满：本页只占这一个） */
    theme_register_change_cb(scan_refresh_theme);

    /* 拉帧定时器（20fps）：回调内部判断页面可见性，隐藏时空转不拉帧 */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
    }
    lv_timer_create(face_status_timer_cb, FACE_STATUS_MS, NULL);
    lv_timer_create(face_users_timer_cb, FACE_USERS_MS, NULL);

    /* 初始化 FPS 窗口起点 */
    s_fps_win_start = hal_time_ms();
    s_video_frames  = 0;
    s_ui_frames     = 0;
    s_video_fps     = 0;
    s_ui_fps        = 0;

    /* 首次刷新标题 / 状态 */
    face_update_header();
    face_status_timer_cb(NULL);

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

    compute_video_rect(w, h);      /* 先算视频矩形：画布尺寸/映射表都依赖它 */
    rebuild_maps();
    rebuild_canvas(w, h);
    reposition_corners_and_beam(w, h);
    /* FPS 角标：右上角，按面板宽度自适配 */
    if (s_fps_box) {
        lv_obj_align(s_fps_box, LV_ALIGN_TOP_RIGHT, -SX(12), SY(10));
    }
}

/* 重建画布缓冲：**尺寸 = 视频矩形**（不是整个面板），并把画布定位到居中位置。
 * 为什么不再满铺面板：满铺时「视频区之外」的留白每帧都要重新刷一次底色，而这块
 * 区域只要在尺寸/主题变化时才需要刷。改成画布 = 视频矩形后，留白交给 s_preview
 * 自己的底色承担，每帧零额外开销，画布缓冲也从约 827KB 降到约 259KB。
 * 注意：本函数必须在 compute_video_rect() 之后调用。 */
static void rebuild_canvas(int32_t w, int32_t h)
{
    (void)w; (void)h;                 /* 尺寸取自 s_vid_*，面板尺寸不再直接使用 */
    int32_t cw = s_vid_w;
    int32_t ch = s_vid_h;
    if (cw <= 0 || ch <= 0) return;

    if (cw == s_canvas_w && ch == s_canvas_h && s_canvas_buf != NULL) {
        /* 尺寸没变：只同步位置（s_vid_x/y 可能随面板尺寸变过），不重新分配 */
        if (s_canvas) {
            lv_obj_set_size(s_canvas, cw, ch);
            lv_obj_set_pos(s_canvas, s_vid_x, s_vid_y);
        }
        return;
    }

    size_t bytes = (size_t)cw * (size_t)ch * 2u;   /* RGB565 */
    uint16_t * nb = (uint16_t *)malloc(bytes);
    if (nb == NULL) {
        printf("[FACE] canvas 缓冲分配失败 (%u bytes)，保持旧尺寸\n", (unsigned)bytes);
        return;
    }
    /* 初始化为面板底色（首帧到达前 / 无摄像头时透出同一底色，视觉上自然） */
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

/* 计算 4:3 视频矩形：等比缩放取最大（放大封顶 FACE_MAX_SCALE），居中（不拉伸变形）。 */
static void compute_video_rect(int32_t cw, int32_t ch)
{
    int32_t avail_h = ch;                     /* 视频最大化 */

    int32_t sx = (int32_t)(((int64_t)cw * 1000) / CAP_W);
    int32_t sy = (int32_t)(((int64_t)avail_h * 1000) / CAP_H);
    int32_t scale = (sx < sy) ? sx : sy;
    if (scale < 1) scale = 1;
    if (scale > FACE_MAX_SCALE) scale = FACE_MAX_SCALE;   /* 放大封顶：见 FACE_MAX_SCALE 注释 */

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

/* 把四角扫描框 + 扫描光带重定位到当前**视频区**（s_vid_*，非整个面板）。 */
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

    /* 占位重新居中（panel 缩放后位置失效） */
    if (s_ph) lv_obj_center(s_ph);

    /* 重定位不影响颜色，但统一按当前状态刷一次，保证状态色不丢 */
    scan_apply_color();
}

/* ===== 双 FPS 窗口更新：每 500ms 各算一次，更新两个角标 ===== */
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
        char buf[24];
        if (s_fps_ui) {
            snprintf(buf, sizeof(buf), "UI %d fps", s_ui_fps);
            lv_label_set_text(s_fps_ui, buf);
        }
        if (s_fps_vid) {
            snprintf(buf, sizeof(buf), "视频 %d fps", s_video_fps);
            lv_label_set_text(s_fps_vid, buf);
        }
    }
}

/* ===== 拉帧定时器：拉帧 → 最近邻缩放 → invalidate 视频区 ===== */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_page_root == NULL || lv_obj_is_hidden(s_page_root)) {
        /* 离开人脸页：状态复位（否则下次进来还挂着上一次的红/绿） */
        if (s_scan_state != PAGE_FACE_SCAN_IDLE) page_face_set_scan_state(PAGE_FACE_SCAN_IDLE);
        return;
    }
    if (!s_canvas || s_canvas_w <= 0 || s_canvas_h <= 0) return;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    /* 预览帧由 face 线程采集 + 转换（规约 §3.4），这里只取最新一帧渲染；
     * 本回调运行在主线程，取回后可直接操作 LVGL，不跨线程。无新帧时保持上一帧。 */
    hal_camera_frame_info_t info;
    if (face_thread_get_preview(s_cap_buf, &info)) {
        /* 1. 帧已在 s_cap_buf（face 线程双帧缓冲拷贝而来），无需再拷贝 */

        /* 2. 最近邻缩放 320×240 → 画布（画布 == 居中的 4:3 视频矩形），查表，
         *    绘制期零除法。画布已收缩到视频矩形，因此**没有留白要填**。 */
        int32_t dst_stride = s_canvas_w;
        uint16_t * buf = s_canvas_buf;
        for (int32_t y = 0; y < s_canvas_h; y++) {
            uint16_t * dst_row = &buf[(size_t)y * dst_stride];
            if (s_map_x && s_map_y) {
                const uint16_t * src_row = &s_cap_buf[(size_t)s_map_y[y] * CAP_W];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    dst_row[x] = src_row[s_map_x[x]];
                }
            } else {
                /* 映射表分配失败时回退：逐像素整型除法 */
                int32_t sy = (int32_t)(((int64_t)y * CAP_H) / s_canvas_h);
                const uint16_t * src_row = &s_cap_buf[sy * CAP_W];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    int32_t sx = (int32_t)(((int64_t)x * CAP_W) / s_canvas_w);
                    dst_row[x] = src_row[sx];
                }
            }
        }

        /* 3. 统计视频帧 + 记录新鲜度 */
        s_video_frames++;
        s_last_frame_ms = hal_time_ms();
        s_got_frame = true;

        /* 4. 隐藏占位 */
        if (s_ph && !lv_obj_is_hidden(s_ph)) lv_obj_set_hidden(s_ph, true);

        /* 画布 == 视频矩形，整块 invalidate 即已是最小重绘面积 */
        lv_obj_invalidate(s_canvas);
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

/* ================================================================
 *  标题 / 状态 / 占位 / 按钮（v2 新增）
 * ================================================================ */

/* 画面区占位：图标 + 一句话 */
static void face_ph_show(bool show, const char * text)
{
    if (s_ph == NULL) return;
    if (text != NULL && s_ph_label != NULL) lv_label_set_text(s_ph_label, text);
    if (show) lv_obj_set_hidden(s_ph, false);
    else      lv_obj_set_hidden(s_ph, true);
}

/* 健康状态：由后端能力 + 运行状态 + 是否有帧推导（真实来源，非写死） */
static const char * face_health_str(void)
{
    const face_caps_t * caps = face_service_caps();
    if (caps == NULL || caps->name == NULL || strcmp(caps->name, "none") == 0) return "未启用";
    if (!face_service_running()) return "异常";
    if (!s_got_frame)            return "待机";
    if (hal_camera_fd() < 0)     return "降级";
    return "正常";
}

/* 标题副文案：后端名 · 模板 n/max · 健康（全部真实来源） */
static void face_update_header(void)
{
    if (s_face_sub == NULL) return;
    const face_caps_t * caps = face_service_caps();
    char buf[112];
    if (caps == NULL || caps->name == NULL || strcmp(caps->name, "none") == 0) {
        snprintf(buf, sizeof(buf), "人脸后端未启用 · 仅显示摄像头预览");
    } else {
        char tpl[24];
        if (caps->max_templates < 0) snprintf(tpl, sizeof(tpl), "%d/不限", s_tpl_count);
        else                         snprintf(tpl, sizeof(tpl), "%d/%d", s_tpl_count, caps->max_templates);
        snprintf(buf, sizeof(buf), "后端 %s · 模板 %s · 健康：%s",
                 caps->name, tpl, face_health_str());
    }
    lv_label_set_text(s_face_sub, buf);
}

/* 按钮可用性：后端不支持该能力 → 置灰（配合状态文案说明） */
static void face_set_btn_enabled(lv_obj_t * btn, bool enabled)
{
    if (btn == NULL) return;
    if (enabled) lv_obj_remove_state(btn, LV_STATE_DISABLED);
    else         lv_obj_add_state(btn, LV_STATE_DISABLED);
}

/* 由最近一次识别结果推导状态文案（结果超过 FACE_RESULT_FRESH_S 秒则忽略） */
static const char * face_status_from_result(void)
{
    const face_result_t * r = face_service_last_result();
    if (r == NULL) return "请将面部对准摄像头";
    uint32_t now = (uint32_t)hal_time();
    if (now - r->timestamp > FACE_RESULT_FRESH_S) return "请将面部对准摄像头";
    switch (r->reason) {
        case FACE_RES_OK:            return "识别成功 · 正在开锁";
        case FACE_RES_NO_MATCH:      return "未匹配 · 请调整角度重试";
        case FACE_RES_LIVENESS_FAIL: return "活体检测未通过 · 请正对摄像头";
        case FACE_RES_TIMEOUT:       return "识别超时 · 请重新对准";
        case FACE_RES_ERROR:         return "模组异常 · 请检查连接";
        default:                     return "请将面部对准摄像头";
    }
}

/* 状态刷新（500ms）：底部状态文案 + 画面区占位 + 按钮可用性 + 标题副文案 */
static void face_status_timer_cb(lv_timer_t * t)
{
    (void)t;

    const face_caps_t * caps = face_service_caps();
    bool backend_none = (caps == NULL || caps->name == NULL ||
                         strcmp(caps->name, "none") == 0);
    bool cam_ok  = (hal_camera_fd() >= 0);
    uint32_t now = hal_time_ms();
    bool live    = s_got_frame && ((now - s_last_frame_ms) < FACE_FRAME_STALE_MS);

    /* ---- 底部状态文案：明确、不空、不显示 "--" ---- */
    const char * st;
    if (!cam_ok)            st = "摄像头未接入 · 预览不可用";
    else if (backend_none)  st = "人脸后端未启用 · 仅预览画面";
    else if (!face_service_running()) st = "人脸后端未运行 · 请检查设备";
    else if (!live)         st = "等待摄像头出帧…";
    else                    st = face_status_from_result();
    if (s_face_status != NULL) lv_label_set_text(s_face_status, st);

    /* ---- 画面区占位 ---- */
    if (live) {
        face_ph_show(false, NULL);
    } else if (backend_none) {
        face_ph_show(true, cam_ok ? "摄像头画面（人脸功能未启用）" : "摄像头未接入，无预览画面");
    } else {
        face_ph_show(true, cam_ok ? "等待摄像头出帧…" : "摄像头未接入，无预览画面");
    }

    /* ---- 按钮可用性（后端能力为准） ---- */
    face_set_btn_enabled(s_btn_enroll, caps != NULL && (caps->caps & FACE_CAP_ENROLL));
    face_set_btn_enabled(s_btn_del,    caps != NULL && (caps->caps & FACE_CAP_DELETE));

    /* ---- 标题副文案（健康可能随帧/运行状态变化） ---- */
    face_update_header();
}

/* 模板数统计（worker 线程读盘，主线程贴值） */
static void face_users_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_users_busy) return;
    s_users_busy = true;
    astore_load_users(face_users_loaded);
}

static void face_users_loaded(safe_user_t * list, int count)
{
    s_users_busy = false;
    int tpl = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].face_id >= 0) tpl++;
    }
    s_tpl_count = tpl;
    face_update_header();
    /* list 由 astore 框架释放 */
}

/* 主题切换：刷新本页新增的本地颜色覆盖（标题/状态/占位走 st_* 自动刷新，
 * 这里只处理按角色取色与自定义底色/文字色的控件） */
static void face_refresh_local_colors(void)
{
    if (s_ph_icon)  lv_obj_set_style_text_color(s_ph_icon, theme_color(TH_TEXT_MUT), 0);
    if (s_ph_label) lv_obj_set_style_text_color(s_ph_label, theme_color(TH_TEXT_MUT), 0);

    lv_obj_t * fps[2] = { s_fps_ui, s_fps_vid };
    for (int i = 0; i < 2; i++) {
        if (fps[i] == NULL) continue;
        lv_obj_set_style_bg_color(fps[i], theme_color(TH_PANEL2), 0);
        lv_obj_set_style_text_color(fps[i], theme_color(TH_TEXT), 0);
    }

    if (s_preview) lv_obj_set_style_border_color(s_preview, theme_color(TH_BORDER), 0);

    if (s_btn_enroll)    lv_obj_set_style_bg_color(s_btn_enroll, theme_color(TH_ACCENT), 0);
    if (s_btn_enroll_ic) lv_obj_set_style_text_color(s_btn_enroll_ic, theme_color(TH_ACCENT_INK), 0);
    if (s_btn_enroll_lb) lv_obj_set_style_text_color(s_btn_enroll_lb, theme_color(TH_ACCENT_INK), 0);
    if (s_btn_del)       lv_obj_set_style_border_color(s_btn_del, theme_color(TH_BORDER), 0);
    if (s_btn_del_ic)    lv_obj_set_style_text_color(s_btn_del_ic, theme_color(TH_TEXT), 0);
    if (s_btn_del_lb)    lv_obj_set_style_text_color(s_btn_del_lb, theme_color(TH_TEXT), 0);
}

/* 录入人脸 / 删除模板：见文件头「安全性说明」——不在本页直接执行，
 * 而是导航到「用户」页并给出明确指引，走已验证的管理员二次验证路径。 */
static void enroll_cb(lv_event_t * e)
{
    (void)e;
    ui_banner("录入人脸：请在「用户」页选择用户（需管理员二次验证）",
              UI_BANNER_INFO, UI_ANIM_BANNER_HOLD_MS_S);
    ui_switch_page(PAGE_USERS);
}

static void delete_tpl_cb(lv_event_t * e)
{
    (void)e;
    ui_banner("删除人脸模板：请在「用户」页选择用户（需管理员二次验证）",
              UI_BANNER_INFO, UI_ANIM_BANNER_HOLD_MS_S);
    ui_switch_page(PAGE_USERS);
}
