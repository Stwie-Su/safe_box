/**
 * @file page_system.c
 * 系统（ui_redesign_preview_v2.html · #sc-sys）：需管理员权限。
 *
 * 布局（三层，从上到下）：
 *   ① 标题行：标题 + 副标题 + 实时时钟（hal_time，非占位）
 *   ② 双栏 body（flex_grow=1）：
 *        左「安全策略」卡：策略步进器 + 只读项 + 保存
 *        右「主题」卡    ：5 套主题色块（取 THEMES[] token）+ 当前主题名
 *   ③ 危险区卡片（独立于 body，作 root 的第三个 flex 子项）
 *
 * ★ 本页历史上有一个缺陷：「恢复出厂设置」按钮会被卡内滚动区推到折叠线以下、
 *   在部分缩放下被裁掉不可点。根因是**面板本身可滚动**（lv_obj 默认带
 *   LV_OBJ_FLAG_SCROLLABLE），内容一旦超高就变成"内部滚动"，按钮跑到折叠线外。
 *   结构性修法（不靠调数字）：
 *     - 左右两张面板一律 lv_obj_set_scrollable(panel, false)（v9 推荐 setter；
 *       弃用的 lv_obj_remove_flag 已由 D3 清理，本页不再使用）；
 *     - 主题列表放进**独立的可滚动子容器**，溢出只滚它；
 *     - 危险区**移出面板**，作为 root 的独立卡片 —— 它不参与任何滚动，
 *       只要 root 是 flex 且 body 带 flex_grow，它就被结构性地保证可见可点。
 *
 * 颜色纪律：不写任何 hex。需要取色时一律走 theme_color(TH_*) 或 THEMES[].pal
 * （后者用于"主题预览色块"这类必须显示**别套主题**颜色的场景）。
 */
#include "page_system.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/config.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "hal/hal_time.h"      /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include "app_version.h"       /* SAFE_VERSION_STRING：版本卡 */
#include <string.h>
#include <time.h>
#include <stdio.h>

static lv_obj_t * s_clock_lbl;
static lv_obj_t * s_failed_lbl;
static lv_obj_t * s_lock_lbl;
static lv_obj_t * s_otp_lbl;
static lv_obj_t * s_theme_btns[THEME_COUNT];
static lv_obj_t * s_theme_cur;      /* 当前主题名（切换后刷新） */

static lv_obj_t * s_ov = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_msg = NULL;

static void clock_timer_cb(lv_timer_t * t);
static void clock_timer_cb(lv_timer_t * t);
static void policy_dec_cb(lv_event_t * e);
static void policy_inc_cb(lv_event_t * e);
static void save_policy_cb(lv_event_t * e);
static void policy_saved_done(int result);
static void theme_click_cb(lv_event_t * e);
static void theme_change_refresh(int idx);
static void factory_worker(void * p);
static void factory_done(void * p);
static void factory_btn_cb(lv_event_t * e);
static void refresh_policy(void);

/* ---- 管理员二次验证（操作处鉴权，FR-7 2026-09-14 变更）----
 * 原设计把鉴权放在「设置中枢」页级入口；但主导航本就直接暴露 系统/用户/网络，
 * 页级验证可被 rail 绕过（鉴权口径不一致）。中枢取消后，鉴权下沉到**敏感操作处**：
 *   保存安全策略 / 恢复出厂设置 → 先验管理员 PIN，通过才执行。
 * 弹窗实现与 page_users 的管理员验证同源（astore_verify_admin，PBKDF2 后台校验）。 */
typedef enum {
    ADMIN_ACT_NONE = 0,
    ADMIN_ACT_SAVE_POLICY,
    ADMIN_ACT_FACTORY,
} admin_action_t;

static void admin_verify_open(admin_action_t act);
static void admin_verify_close(void);
static void admin_verify_dispatch(void);
static void admin_verify_key_cb(lv_event_t * e);
static void admin_verify_cancel_cb(lv_event_t * e);
static void admin_verify_ok_cb(lv_event_t * e);
static void admin_verify_update(void);
static void admin_verify_done(int r);

static void dlg_factory(void);
static void close_dlg(void);
static void dlg_cancel_cb(lv_event_t * e);
static void do_factory_cb(lv_event_t * e);

static int s_pending_max_failed = 5;
static int s_pending_lock_secs  = 60;

static admin_action_t s_admin_act = ADMIN_ACT_NONE;
static lv_obj_t * s_av_ov   = NULL;
static lv_obj_t * s_av_win = NULL;
static lv_obj_t * s_av_disp = NULL;
static lv_obj_t * s_av_msg  = NULL;
static char s_av_pin[16] = {0};

/* ---------------- 小组件 ---------------- */

/** 只读信息行：左灰标签、右值（值可后续 set_text 刷新）。 */
static lv_obj_t * info_row(lv_obj_t * parent, const char * name, lv_obj_t ** val_out)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t * k = lv_label_create(row);
    lv_label_set_text(k, name);
    lv_obj_add_style(k, &st_text_mut, 0);
    lv_obj_set_style_text_font(k, app_font_scaled(14), 0);

    lv_obj_t * v = lv_label_create(row);
    lv_label_set_text(v, "--");
    lv_obj_add_style(v, &st_text, 0);
    lv_obj_set_style_text_font(v, app_font_scaled(14), 0);
    if (val_out) *val_out = v;
    return row;
}

/* 策略步进行：label + [-] 值 [+]；which=0 失败次数，1 锁定时长 */
static lv_obj_t * stepper_row(lv_obj_t * parent, const char *name, lv_obj_t **val_lbl,
                              lv_event_cb_t dec_cb, lv_event_cb_t inc_cb, int which)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_add_style(row, &st_panel2, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * nm = lv_label_create(row);
    lv_label_set_text(nm, name);
    lv_obj_add_style(nm, &st_text, 0);
    lv_obj_set_style_text_font(nm, app_font_scaled(15), 0);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_set_style_pad_left(nm, 12, 0);

    lv_obj_t * dec = lv_button_create(row);
    lv_obj_set_size(dec, 40, 34);
    lv_obj_add_style(dec, &st_ghost_btn, 0);
    lv_obj_add_event_cb(dec, dec_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)which);
    lv_obj_t * decl = lv_label_create(dec);
    lv_label_set_text(decl, "−");
    lv_obj_set_style_text_font(decl, app_font_scaled(18), 0);
    lv_obj_center(decl);

    *val_lbl = lv_label_create(row);
    lv_label_set_text(*val_lbl, "--");
    lv_obj_add_style(*val_lbl, &st_text, 0);
    lv_obj_set_style_text_font(*val_lbl, app_font_scaled(16), 0);
    lv_obj_set_style_pad_left(*val_lbl, 12, 0);
    lv_obj_set_style_pad_right(*val_lbl, 12, 0);

    lv_obj_t * inc = lv_button_create(row);
    lv_obj_set_size(inc, 40, 34);
    lv_obj_add_style(inc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(inc, inc_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)which);
    lv_obj_t * incl = lv_label_create(inc);
    lv_label_set_text(incl, "+");
    lv_obj_set_style_text_font(incl, app_font_scaled(18), 0);
    lv_obj_center(incl);

    lv_obj_set_style_pad_right(row, 10, 0);
    return row;
}

/** 主题项：色块（直接用该主题的 token 取色）+ 名称；选中态靠 CHECKED 样式。 */
static lv_obj_t * theme_item(lv_obj_t * parent, int idx)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_set_size(b, lv_pct(100), 42);
    lv_obj_add_style(b, &st_panel2, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_style(b, &st_accent_btn, LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, theme_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(b, 12, 0);

    /* 色块：左半为该主题的强调色，右半为面板色 —— 一眼看出"这套主题长什么样" */
    lv_obj_t * sw = lv_obj_create(b);
    lv_obj_set_size(sw, 34, 24);
    lv_obj_set_style_radius(sw, 6, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_set_scrollable(sw, false);
    lv_obj_set_style_bg_color(sw, THEMES[idx].pal.c[TH_ACCENT], 0);

    lv_obj_t * lb = lv_label_create(b);
    lv_label_set_text(lb, theme_name(idx));
    lv_obj_add_style(lb, &st_text, 0);
    lv_obj_set_style_text_font(lb, app_font_scaled(15), 0);
    lv_obj_set_style_pad_left(lb, 10, 0);
    return b;
}

/* ---------------- 页面 ---------------- */

lv_obj_t * page_system_create(lv_obj_t * parent)
{
    const safe_policy_t * pol = user_policy();
    s_pending_max_failed = pol->max_failed;
    s_pending_lock_secs  = pol->lock_seconds;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, SX(18), 0);
    lv_obj_set_style_pad_row(root, SY(10), 0);

    /* ---------- ① 标题行 ---------- */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    /* rail 即导航，本页不放「返回」（与 v2 预览一致；原返回目标「设置中枢」已取消） */
    lv_obj_t * tt = lv_obj_create(head);
    lv_obj_set_size(tt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tt, 0, 0);
    lv_obj_set_style_pad_all(tt, 0, 0);
    lv_obj_set_scrollable(tt, false);
    lv_obj_set_flex_flow(tt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(tt, 14, 0);
    lv_obj_set_style_pad_row(tt, 2, 0);

    lv_obj_t * title = lv_label_create(tt);
    lv_label_set_text(title, "系统");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(26), 0);

    lv_obj_t * subtitle = lv_label_create(tt);
    lv_label_set_text(subtitle, "需管理员权限");
    lv_obj_add_style(subtitle, &st_text_mut, 0);
    lv_obj_set_style_text_font(subtitle, app_font_scaled(12), 0);

    lv_obj_t * clock_wrap = lv_obj_create(head);
    lv_obj_set_flex_grow(clock_wrap, 1);
    lv_obj_set_height(clock_wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(clock_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_wrap, 0, 0);
    lv_obj_set_style_pad_all(clock_wrap, 0, 0);
    lv_obj_set_scrollable(clock_wrap, false);
    lv_obj_set_flex_flow(clock_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_wrap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    s_clock_lbl = lv_label_create(clock_wrap);
    lv_label_set_text(s_clock_lbl, "--");
    lv_obj_add_style(s_clock_lbl, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_clock_lbl, app_font_scaled(14), 0);

    /* ---------- ② 双栏 ---------- */
    lv_obj_t * body = lv_obj_create(root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, SX(14), 0);

    /* --- 左：安全策略 --- */
    lv_obj_t * left = lv_obj_create(body);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_add_style(left, &st_panel, 0);
    lv_obj_set_scrollable(left, false);   /* ★ 面板不自滚动 */
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);

    lv_obj_t * lt = lv_label_create(left);
    lv_label_set_text(lt, "安全策略");
    lv_obj_add_style(lt, &st_text, 0);
    lv_obj_set_style_text_font(lt, app_font_scaled(19), 0);

    stepper_row(left, "连续失败次数上限", &s_failed_lbl, policy_dec_cb, policy_inc_cb, 0);
    stepper_row(left, "锁定时长（秒）",   &s_lock_lbl,   policy_dec_cb, policy_inc_cb, 1);

    /* 只读项：数据来自 user_policy()，非占位 */
    lv_obj_t * sep = lv_obj_create(left);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, theme_color(TH_BORDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_scrollable(sep, false);

    info_row(left, "人脸连续未匹配转动态码", &s_otp_lbl);
    lv_obj_t * vpin = NULL;
    info_row(left, "虚位密码", &vpin);
    if (vpin) {
        bool en = pol->virtual_pin_enable;
        lv_label_set_text(vpin, en ? "已启用" : "已停用");
        lv_obj_add_style(vpin, en ? &st_ok_text : &st_warn_text, 0);
    }
    lv_obj_t * verify_t = NULL;
    info_row(left, "人脸验证超时（秒）", &verify_t);
    if (verify_t) {
        char b[16];
        snprintf(b, sizeof(b), "%d", pol->face_verify_timeout_s);
        lv_label_set_text(verify_t, b);
    }
    lv_obj_t * fw = NULL;
    info_row(left, "固件版本", &fw);
    if (fw) lv_label_set_text(fw, SAFE_VERSION_STRING);

    /* 中间弹性占位：把保存按钮压到卡底（面板不自滚动，故不会被裁） */
    lv_obj_t * lspacer = lv_obj_create(left);
    lv_obj_set_width(lspacer, lv_pct(100));
    lv_obj_set_flex_grow(lspacer, 1);
    lv_obj_set_style_bg_opa(lspacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lspacer, 0, 0);
    lv_obj_set_scrollable(lspacer, false);

    lv_obj_t * save = ui_icon_text_button(left, LV_SYMBOL_SAVE, "保存策略",
                                          lv_pct(100), SY(42), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), save_policy_cb, NULL);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);

    /* --- 右：主题 --- */
    lv_obj_t * right = lv_obj_create(body);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_add_style(right, &st_panel, 0);
    lv_obj_set_scrollable(right, false);  /* ★ 面板不自滚动 */
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);

    lv_obj_t * rt = lv_label_create(right);
    lv_label_set_text(rt, "主题");
    lv_obj_add_style(rt, &st_text, 0);
    lv_obj_set_style_text_font(rt, app_font_scaled(19), 0);

    /* 主题列表：唯一可滚动的地方，溢出只滚它，不影响其它元素 */
    lv_obj_t * tlist = lv_obj_create(right);
    lv_obj_set_width(tlist, lv_pct(100));
    lv_obj_set_flex_grow(tlist, 1);
    lv_obj_set_style_bg_opa(tlist, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tlist, 0, 0);
    lv_obj_set_style_pad_all(tlist, 0, 0);
    lv_obj_set_style_pad_right(tlist, 6, 0);
    lv_obj_set_flex_flow(tlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tlist, 6, 0);

    for (int i = 0; i < THEME_COUNT; i++) {
        s_theme_btns[i] = theme_item(tlist, i);
        if (i == theme_idx()) lv_obj_add_state(s_theme_btns[i], LV_STATE_CHECKED);
    }

    /* 外部切主题（如调试钩子 / 将来其它入口）时同步选中态与「当前」文案。
     * 只在本页内点选的话 theme_click_cb 已即时更新，但 theme_switch() 是从别处
     * 调用的——那时本页不会被重建，必须靠 theme 的变更回调来刷新，
     * 否则会出现「实际是石墨黑、面板却写着浅蓝」的错位。 */
    theme_register_change_cb(theme_change_refresh);

    s_theme_cur = lv_label_create(right);
    lv_obj_add_style(s_theme_cur, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_theme_cur, app_font_scaled(12), 0);

    /* ---------- ③ 危险区：独立卡片，不参与任何滚动滚动 ---------- */
    lv_obj_t * danger = lv_obj_create(root);
    lv_obj_set_size(danger, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_style(danger, &st_panel, 0);
    lv_obj_set_style_pad_all(danger, SX(12), 0);
    lv_obj_set_scrollable(danger, false);
    lv_obj_set_flex_flow(danger, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(danger, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t * dtxt = lv_obj_create(danger);
    lv_obj_set_flex_grow(dtxt, 1);
    lv_obj_set_height(dtxt, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dtxt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dtxt, 0, 0);
    lv_obj_set_style_pad_all(dtxt, 0, 0);
    lv_obj_set_scrollable(dtxt, false);
    lv_obj_set_flex_flow(dtxt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dtxt, 2, 0);

    lv_obj_t * dh = lv_label_create(dtxt);
    lv_label_set_text(dh, "危险区");
    lv_obj_add_style(dh, &st_danger_text, 0);
    lv_obj_set_style_text_font(dh, app_font_scaled(15), 0);

    lv_obj_t * dd = lv_label_create(dtxt);
    lv_label_set_text(dd, "恢复出厂设置将清空全部用户、日志与配置，且不可撤销");
    lv_obj_add_style(dd, &st_text_mut, 0);
    lv_obj_set_style_text_font(dd, app_font_scaled(12), 0);

    lv_obj_t * fr = ui_icon_text_button(danger, LV_SYMBOL_TRASH, "恢复出厂设置",
                                        SX(190), SY(40), &st_danger_btn,
                                        lv_color_white(), factory_btn_cb, NULL);
    (void)fr;

    refresh_policy();
    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
    return root;
}

static void clock_timer_cb(lv_timer_t * t)
{
    (void)t;
    char buf[40];
    time_t now = (time_t)hal_time();
    struct tm * tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmv);
        lv_label_set_text(s_clock_lbl, buf);
    }
}

static void refresh_policy(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", s_pending_max_failed);
    lv_label_set_text(s_failed_lbl, buf);
    snprintf(buf, sizeof(buf), "%d", s_pending_lock_secs);
    lv_label_set_text(s_lock_lbl, buf);
    if (s_otp_lbl) {
        const safe_policy_t * pol = user_policy();
        snprintf(buf, sizeof(buf), "%d 次", pol->face_otp_after);
        lv_label_set_text(s_otp_lbl, buf);
    }
    if (s_theme_cur) {
        char tb[48];
        snprintf(tb, sizeof(tb), "当前：%s", theme_name(theme_idx()));
        lv_label_set_text(s_theme_cur, tb);
    }
}

static void policy_dec_cb(lv_event_t * e)
{
    int which = (int)(uintptr_t)lv_event_get_user_data(e);
    if (which == 0) {
        if (s_pending_max_failed > 1) s_pending_max_failed--;
    } else {
        if (s_pending_lock_secs > 5) s_pending_lock_secs -= 5;
    }
    refresh_policy();
}

static void policy_inc_cb(lv_event_t * e)
{
    int which = (int)(uintptr_t)lv_event_get_user_data(e);
    if (which == 0) {
        if (s_pending_max_failed < 10) s_pending_max_failed++;
    } else {
        if (s_pending_lock_secs < 3600) s_pending_lock_secs += 5;
    }
    refresh_policy();
}

static void save_policy_cb(lv_event_t * e)
{
    (void)e;
    /* FR-7（2026-09-14 变更）：策略修改属敏感操作，先验管理员 PIN，通过才落盘 */
    admin_verify_open(ADMIN_ACT_SAVE_POLICY);
}

static void policy_saved_done(int result)
{
    (void)result;
    astore_append_log("setting_change", "admin", 1, "policy updated");
}

static void theme_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= THEME_COUNT) return;
    theme_switch(idx);          /* 统一由变更回调刷新 UI，避免两处逻辑漂移 */
    astore_append_log("setting_change", "admin", 1, theme_name(idx));
}

/** theme_switch() 的变更回调：把选中态与「当前：X」刷成给定主题。 */
static void theme_change_refresh(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return;
    for (int i = 0; i < THEME_COUNT; i++) {
        if (!s_theme_btns[i]) continue;
        if (i == idx) lv_obj_add_state(s_theme_btns[i], LV_STATE_CHECKED);
        else          lv_obj_remove_state(s_theme_btns[i], LV_STATE_CHECKED);
    }
    if (s_theme_cur) {
        char tb[48];
        snprintf(tb, sizeof(tb), "当前：%s", theme_name(idx));
        lv_label_set_text(s_theme_cur, tb);
    }
}

/* ---------------- 恢复出厂 ---------------- */
static void factory_btn_cb(lv_event_t * e)
{
    (void)e;
    /* FR-7（2026-09-14 变更）：恢复出厂属敏感操作，先验管理员 PIN，通过再弹确认框 */
    admin_verify_open(ADMIN_ACT_FACTORY);
}

static void close_dlg(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    s_ov = NULL; s_win = NULL; s_msg = NULL;
}

static void dlg_cancel_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

static void dlg_factory(void)
{
    close_dlg();
    lv_obj_t * scr = lv_screen_active();
    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_scrollable(s_ov, false);

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, 380, 200);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    lv_obj_center(s_win);
    lv_obj_set_scrollable(s_win, false);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 10, 0);

    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, "恢复出厂将清除全部用户 / 网络 / 日志");
    lv_obj_add_style(t, &st_danger_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(16), 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位（见 page_users 同样注释） */
    const int32_t btn_y = lv_obj_get_height(s_win) - 16 - 44 - 16;
    const int32_t btn_x = (lv_obj_get_width(s_win) - 32 - 280) / 2 + 16;

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 136, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_set_floating(cc, true);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font_scaled(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * yy = ui_icon_text_button(s_win, LV_SYMBOL_TRASH, "确认恢复",
                                        136, 44, &st_danger_btn,
                                        lv_color_white(), do_factory_cb, NULL);
    lv_obj_set_floating(yy, true);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
}

/* 恢复出厂：删文件 + 重建默认 admin 整段在后台执行 */
static void factory_worker(void * p)
{
    (void)p;
    char path[560];
    snprintf(path, sizeof(path), "%susers.json", store_dir());
    remove(path);
    snprintf(path, sizeof(path), "%snetwork.json", store_dir());
    remove(path);
    snprintf(path, sizeof(path), "%ssafe.log", store_dir());
    remove(path);

    store_init();   /* 重建默认 admin */
    log_append("factory_reset", "system", 1, "all data cleared");
}

static void factory_done(void * p)
{
    (void)p;
    close_dlg();
}

static void do_factory_cb(lv_event_t * e)
{
    (void)e;
    worker_post(factory_worker, NULL, factory_done);
}

/* ================= 管理员二次验证弹窗（操作处鉴权） =================
 * 布局与 page_users 的管理员验证同款（绝对定位，不用 flex，避免按钮被压扁）。
 * 校验走 astore_verify_admin()：PBKDF2 在后台线程算，结果回主线程处理；
 * 通过后按 s_admin_act 分派到真正的动作（保存策略 / 打开恢复出厂确认框）。 */

static void admin_verify_open(admin_action_t act)
{
    admin_verify_close();
    s_admin_act = act;
    s_av_pin[0] = '\0';

    lv_obj_t * scr = lv_screen_active();
    s_av_ov = lv_obj_create(scr);
    lv_obj_set_size(s_av_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_av_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_av_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_av_ov, 0, 0);
    lv_obj_set_scrollable(s_av_ov, false);

    s_av_win = lv_obj_create(s_av_ov);
    lv_obj_set_size(s_av_win, 380, 460);
    lv_obj_add_style(s_av_win, &st_panel, 0);
    lv_obj_set_style_radius(s_av_win, SX(16), 0);
    lv_obj_set_style_pad_all(s_av_win, 0, 0);
    lv_obj_set_scrollable(s_av_win, false);
    lv_obj_center(s_av_win);

    lv_obj_t * t = lv_label_create(s_av_win);
    lv_label_set_text(t, "管理员验证");
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    lv_obj_set_width(t, 380);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, 14);

    s_av_disp = lv_label_create(s_av_win);
    lv_label_set_text(s_av_disp, "——");
    lv_obj_add_style(s_av_disp, &st_text, 0);
    lv_obj_set_style_text_font(s_av_disp, app_font_scaled(28), 0);
    lv_obj_set_style_pad_all(s_av_disp, 0, 0);
    lv_obj_set_style_text_align(s_av_disp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_av_disp, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_av_disp, 8, 0);
    lv_obj_set_size(s_av_disp, 240, 50);
    lv_obj_set_pos(s_av_disp, 70, 54);

    s_av_msg = lv_label_create(s_av_win);
    lv_label_set_text(s_av_msg, "请输入管理员 PIN");
    lv_obj_add_style(s_av_msg, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_av_msg, app_font_scaled(14), 0);
    lv_obj_set_width(s_av_msg, 380);
    lv_obj_set_style_text_align(s_av_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_av_msg, 0, 116);

    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    const int ROW_Y[4] = { 160, 210, 260, 310 };
    const int KEY_X[3] = { 50, 146, 242 };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(s_av_win);
            lv_obj_set_size(k, 88, 40);
            lv_obj_set_pos(k, KEY_X[c], ROW_Y[r]);
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, 8, 0);
            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(16), 0);
            lv_obj_center(kl);
            lv_obj_add_event_cb(k, admin_verify_key_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(r * 3 + c));
        }
    }

    lv_obj_t * cancel = lv_button_create(s_av_win);
    lv_obj_set_size(cancel, 136, 40);
    lv_obj_set_pos(cancel, 50, 360);
    lv_obj_add_style(cancel, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cancel, admin_verify_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, app_font_scaled(16), 0);
    lv_obj_center(cl);

    lv_obj_t * ok = lv_button_create(s_av_win);
    lv_obj_set_size(ok, 136, 40);
    lv_obj_set_pos(ok, 194, 360);
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, admin_verify_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ol = lv_label_create(ok);
    lv_label_set_text(ol, "确认");
    lv_obj_set_style_text_font(ol, app_font_scaled(16), 0);
    lv_obj_center(ol);

    admin_verify_update();
}

static void admin_verify_close(void)
{
    if (s_av_ov) lv_obj_delete(s_av_ov);
    s_av_ov = NULL; s_av_disp = NULL; s_av_msg = NULL;
    s_av_pin[0] = '\0';
}

static void admin_verify_update(void)
{
    size_t len = strlen(s_av_pin);
    char masked[16];
    for (size_t i = 0; i < len; i++) masked[i] = '*';
    masked[len] = '\0';
    if (s_av_disp) lv_label_set_text(s_av_disp, len ? masked : "——");
}

static void admin_verify_key_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    static const char * KEYS[12] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "退格", "0", "清空",
    };
    const char * key = KEYS[idx];
    if (strcmp(key, "清空") == 0) {
        s_av_pin[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_av_pin);
        if (len > 0) s_av_pin[len - 1] = '\0';
    } else {
        size_t len = strlen(s_av_pin);
        if (len < 8) { s_av_pin[len] = key[0]; s_av_pin[len + 1] = '\0'; }
    }
    admin_verify_update();
}

static void admin_verify_cancel_cb(lv_event_t * e)
{
    (void)e;
    s_admin_act = ADMIN_ACT_NONE;
    admin_verify_close();
}

static void admin_verify_done(int r)
{
    if (r == 0) {
        astore_append_log("setting_change", "admin", 1, "admin verify ok");
        admin_verify_close();
        admin_verify_dispatch();
    } else if (r == 2) {
        if (s_av_msg) lv_label_set_text(s_av_msg, "已被锁定，请稍后再试");
        s_av_pin[0] = '\0';
        admin_verify_update();
    } else {
        if (s_av_msg) lv_label_set_text(s_av_msg, "PIN 错误，请重试");
        s_av_pin[0] = '\0';
        admin_verify_update();
    }
}

static void admin_verify_ok_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_av_pin) < 4) {
        if (s_av_msg) lv_label_set_text(s_av_msg, "PIN 至少 4 位");
        return;
    }
    astore_verify_admin(s_av_pin, admin_verify_done);
}

static void admin_verify_dispatch(void)
{
    switch (s_admin_act) {
    case ADMIN_ACT_SAVE_POLICY:
        /* 策略落盘（重写 users.json）放后台；日志异步追加 */
        astore_set_policy(s_pending_max_failed, s_pending_lock_secs, policy_saved_done);
        break;
    case ADMIN_ACT_FACTORY:
        dlg_factory();          /* 验证通过后再弹恢复出厂确认框（破坏性操作双重确认） */
        break;
    case ADMIN_ACT_NONE:
    default:
        break;
    }
    s_admin_act = ADMIN_ACT_NONE;
}
