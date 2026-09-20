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
#include "core/event_bus.h"      /* 订阅 EV_FACE_EVENT：识别「全清完成」 */
#include "hal/hal_face.h"        /* face_service_delete_all_async */
#include "ui/ui_feedback.h"      /* ui_banner：操作结果横幅 */
#include "hal/hal_time.h"      /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include "app_version.h"       /* SAFE_VERSION_STRING：版本卡 */
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>     /* calloc/free：虚位开关的异步落盘作业 */

static lv_obj_t * s_clock_lbl;
static lv_obj_t * s_failed_lbl;
static lv_obj_t * s_lock_lbl;
static lv_obj_t * s_otp_lbl;
static lv_obj_t * s_vpin_sw;        /* 虚位密码开关（FR-18）—— 可点，不再是只读 info_row */
static bool       s_vpin_pending = false;   /* 待写入的开关目标值（管理员验证通过后才落盘） */
static lv_obj_t * s_mode_sw;        /* 人脸录入模式：单帧 / 五向 */
static bool       s_mode_pending = false;   /* 待写入的目标值（管理员验证通过后才落盘） */
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

/* 虚位密码开关（FR-18）：落盘走后台线程，不在 UI 线程做文件 IO */
static lv_obj_t * toggle_row(lv_obj_t * parent, const char * name, const char * hint,
                             lv_obj_t ** sw_out, lv_event_cb_t cb);
static void vpin_click_cb(lv_event_t * e);
static void vpin_refresh(void);
static void vpin_write_async(bool enable);
static void mode_click_cb(lv_event_t * e);
static void mode_refresh(void);
static void mode_write_async(bool five_way);

/* ---- 管理员二次验证（操作处鉴权，FR-7 2026-09-14 变更）----
 * 原设计把鉴权放在「设置中枢」页级入口；但主导航本就直接暴露 系统/用户/网络，
 * 页级验证可被 rail 绕过（鉴权口径不一致）。中枢取消后，鉴权下沉到**敏感操作处**：
 *   保存安全策略 / 恢复出厂设置 → 先验管理员 PIN，通过才执行。
 * 弹窗实现与 page_users 的管理员验证同源（astore_verify_admin，PBKDF2 后台校验）。 */
typedef enum {
    ADMIN_ACT_NONE = 0,
    ADMIN_ACT_SAVE_POLICY,
    ADMIN_ACT_FACTORY,
    ADMIN_ACT_VIRTUAL_PIN,
    ADMIN_ACT_CLEAR_FACE,      /* 清空模组人脸：模组被外部写过之后的「拉回一致」手段 */
    ADMIN_ACT_ENROLL_MODE,     /* 切换人脸录入模式（单帧/五向） */
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
static void dlg_clear_face(void);
static void clear_face_btn_cb(lv_event_t * e);
static void do_clear_face_cb(lv_event_t * e);
/* 模组「全清」应答处理（定义在后；创建页面时要注册订阅） */
static void on_face_event_sys(ev_topic_t topic, const void * payload, void * user);
static void clear_face_local_done(void * p);
static void close_dlg(void);
static void dlg_cancel_cb(lv_event_t * e);
static void do_factory_cb(lv_event_t * e);

static int s_pending_max_failed = 5;
static int s_pending_lock_secs  = 60;

static admin_action_t s_admin_act = ADMIN_ACT_NONE;
/* 事件订阅幂等标志：本页可能被反复创建，重复订阅会累积回调 */
static bool s_sys_subscribed = false;
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
    lv_obj_set_scrollable(row, false);   /* 同「页面自滚动」根因：纯布局容器一律关滚动 */
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

/**
 * @brief 可点开关行：左侧「主标签 + 次要说明」两行，右侧开关按钮。
 *
 * FR-18 的虚位密码原本是一个只读 info_row（只显示「已启用/已停用」），管理员
 * 没有任何入口改它 —— 这里换成真正的开关。
 *
 * 为什么用 lv_button + CHECKED 而不是 lv_switch：
 *   - CHECKED 直接挂 st_ghost_btn / st_accent_btn 两套已有主题样式，换主题自动
 *     跟随，不需要额外注册刷新回调，也不会像 lv_switch 那样引入新的绘制部件；
 *   - 无阴影、无大圆角（圆角只有高度的一半，属按钮常规形态），符合板端帧率要求。
 *
 * @param name    主标签（功能名）
 * @param hint    次要说明（可选，NULL 不显示）
 * @param sw_out  回传开关按钮句柄（可为 NULL）
 */
static lv_obj_t * toggle_row(lv_obj_t * parent, const char * name, const char * hint,
                             lv_obj_t ** sw_out, lv_event_cb_t cb)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 左：主标签 + 说明（信息层次：功能名用正文色，说明用次要色、字号更小） */
    lv_obj_t * col = lv_obj_create(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, SY(1), 0);
    lv_obj_set_scrollable(col, false);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * k = lv_label_create(col);
    lv_label_set_text(k, name);
    lv_obj_add_style(k, &st_text, 0);
    lv_obj_set_style_text_font(k, app_font_scaled(14), 0);

    if (hint) {
        lv_obj_t * h = lv_label_create(col);
        lv_label_set_text(h, hint);
        lv_obj_add_style(h, &st_text_mut, 0);
        lv_obj_set_style_text_font(h, app_font_scaled(11), 0);
    }

    /* 右：开关按钮。文字由各自的 refresh 按真实策略值刷新。
     * 回调由调用方传入 —— 原先写死 vpin_click_cb，第二个使用者（录入模式）
     * 点了不会走自己的逻辑（编译期就是 unused-function 警告，正是它暴露的）。 */
    lv_obj_t * sw = lv_button_create(row);
    lv_obj_set_size(sw, SX(92), SY(30));
    lv_obj_add_style(sw, &st_ghost_btn, 0);
    lv_obj_add_style(sw, &st_accent_btn, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(sw, SX(6), 0);
    lv_obj_set_style_pad_ver(sw, 0, 0);
    lv_obj_set_style_radius(sw, SX(8), 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * sl = lv_label_create(sw);
    lv_label_set_text(sl, "已关闭");
    lv_obj_set_style_text_font(sl, app_font_scaled(13), 0);
    lv_obj_center(sl);

    if (sw_out) *sw_out = sw;
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
    /* 虚位密码（FR-18）：可点开关。改它属「策略修改」= 敏感操作（FR-7），
     * 点击后先验管理员 PIN，通过才在后台线程落盘。 */
    /* 说明里的长度上限取自统一常量，避免策略改了界面还在说旧数字（v1.9：20→12） */
    char vpin_hint[64];
    snprintf(vpin_hint, sizeof(vpin_hint),
             "开启后 PIN 前后可加干扰位（最长 %d 位）", SAFE_VIRTUAL_PIN_MAX_INPUT);
    toggle_row(left, "虚位密码", vpin_hint, &s_vpin_sw, vpin_click_cb);

    /* 人脸录入模式：单帧 / 五向。
     * 放这里（与其它人脸策略同卡）而不是「开发者选项」—— 后者定位是只读诊断。 */
    toggle_row(left, "人脸录入模式",
               "五向＝一次录入内依次采集 正/左/右/上/下；单帧＝只采一次",
               &s_mode_sw, mode_click_cb);
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

    /* 「清空模组人脸」单独成按钮（**不**并入恢复出厂）：恢复出厂是 app 侧概念
     * （清用户/网络/日志），而这条动的是模组侧数据、且不可撤销 —— 混在一起会让
     * 「清 app 数据」意外清掉模组。它的作用是：模组被外部工具单独写过之后，
     * 本地与模组长期不一致（uid 编号空间被两方共用），这条是唯一的「拉回一致」手段。 */
    lv_obj_t * cf = ui_icon_text_button(danger, LV_SYMBOL_TRASH, "清空模组人脸",
                                        SX(190), SY(40), &st_danger_btn,
                                        lv_color_white(), clear_face_btn_cb, NULL);
    (void)cf;

    refresh_policy();
    vpin_refresh();
    mode_refresh();
    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);

    /* 订阅人脸事件：只关心「全清完成」（face_id == -1）。
     * 幂等保护：页面可被反复创建，不判重会累积回调（同一事件触发多次）。 */
    if (!s_sys_subscribed) {
        event_bus_subscribe(EV_FACE_EVENT, on_face_event_sys, NULL);
        s_sys_subscribed = true;
    }
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

/* ---------------- 虚位密码开关（FR-18） ----------------
 * 写路径：点击 → 管理员二次验证（FR-7 敏感操作）→ worker_post 后台写 users.json
 *         → 主线程 done 回调刷新控件。
 * ★ 不在 UI 线程做文件 IO：user_policy_set_virtual_pin() 内部要 load + save
 *   整个 users.json（PBKDF2 之外的纯磁盘操作），放在主线程会卡帧（NFR-8 / NFR-3）。
 * ★ 也刻意不走 async_store：本轮改动范围限定在 UI 与 store 两处，
 *   core/support 目录是别人刚改完的文件，不碰（同样的 worker_post 组合在本工程
 *   的 page_users 里已是既定写法）。 */

typedef struct { bool enable; } vpin_job_t;

static void vpin_worker(void * p)
{
    vpin_job_t * j = (vpin_job_t *)p;
    user_policy_set_virtual_pin(j->enable);
}

static void vpin_done(void * p)
{
    vpin_job_t * j = (vpin_job_t *)p;
    /* 以落盘后的真实策略值刷新界面，而不是直接信 j->enable —— 万一 store 侧
     * 将来加了拒绝条件，界面也会如实反映，不会出现「显示已开启、实际没开」。 */
    vpin_refresh();
    astore_append_log("setting_change", "admin", 1,
                      j->enable ? "virtual_pin on" : "virtual_pin off");
    free(j);
}

/* ---------------- 人脸录入模式（单帧/五向）----------------
 * 完整照抄 vpin 的范式：点击 → 鉴权 → worker 落盘 → done 刷新。
 * 差别只有两处：①落盘后把新值**同步给后端**（策略是唯一真源）；
 * ②按钮文字是「单帧/五向」而不是「已开启/已关闭」。 */
typedef struct { bool five; } mode_job_t;

static void mode_worker(void * p)
{
    mode_job_t * j = (mode_job_t *)p;
    user_policy_set_enroll_mode(j->five);
}

static void mode_done(void * p)
{
    mode_job_t * j = (mode_job_t *)p;
    /* 落盘完成后再同步后端 —— 顺序反了会让界面/后端短暂不一致。 */
    face_service_set_enroll_five_way(j->five);
    mode_refresh();
    astore_append_log("setting_change", "admin", 1,
                      j->five ? "face enroll mode: five-way"
                              : "face enroll mode: single");
    free(j);
}

static void mode_write_async(bool five_way)
{
    mode_job_t * j = (mode_job_t *)calloc(1, sizeof(*j));
    if (!j) return;
    j->five = five_way;
    worker_post(mode_worker, j, mode_done);
}

static void mode_refresh(void)
{
    if (!s_mode_sw) return;
    const safe_policy_t * pol = user_policy();
    bool five = pol->enroll_five_way;

    if (five) lv_obj_add_state(s_mode_sw, LV_STATE_CHECKED);
    else      lv_obj_remove_state(s_mode_sw, LV_STATE_CHECKED);

    lv_obj_t * lb = lv_obj_get_child(s_mode_sw, 0);
    if (lb) lv_label_set_text(lb, five ? "五向" : "单帧");
}

static void mode_click_cb(lv_event_t * e)
{
    (void)e;
    s_mode_pending = !user_policy()->enroll_five_way;   /* 目标值 = 取反 */
    admin_verify_open(ADMIN_ACT_ENROLL_MODE);
}

static void vpin_write_async(bool enable)
{
    vpin_job_t * j = (vpin_job_t *)calloc(1, sizeof(*j));
    if (!j) return;
    j->enable = enable;
    worker_post(vpin_worker, j, vpin_done);
}

/** 按当前生效策略刷新开关的选中态与文字（换主题时会由样式自动跟随颜色）。 */
static void vpin_refresh(void)
{
    if (!s_vpin_sw) return;
    const safe_policy_t * pol = user_policy();
    bool en = pol->virtual_pin_enable;

    if (en) lv_obj_add_state(s_vpin_sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(s_vpin_sw, LV_STATE_CHECKED);

    lv_obj_t * lb = lv_obj_get_child(s_vpin_sw, 0);
    if (lb) lv_label_set_text(lb, en ? "已开启" : "已关闭");
}

static void vpin_click_cb(lv_event_t * e)
{
    (void)e;
    const safe_policy_t * pol = user_policy();
    s_vpin_pending = !pol->virtual_pin_enable;   /* 目标值 = 取反 */
    admin_verify_open(ADMIN_ACT_VIRTUAL_PIN);
}

/* ---------------- 清空模组人脸 ---------------- */

static void dlg_clear_face(void);
static void clear_face_btn_cb(lv_event_t * e);
static void do_clear_face_cb(lv_event_t * e);

/* 本地绑定同步清空 —— **必须走 worker**：这是 store 写操作（重写 users.json），
 * 主线程直接调会破坏「文件 IO 由 worker 单线程持有」的不变式（QA-20）。 */
static void clear_face_local_worker(void * p)
{
    (void)p;
    user_face_clear_all();
}

/* 写盘完成后的主线程回调：**广播必须放在这里**，不能放在 worker_post 之后立即发 ——
 * 否则 USERS 页会抢在写盘完成前 reload，读到的还是旧数据（按钮状态不变）。 */
static void clear_face_local_done(void * p)
{
    (void)p;
    event_bus_publish(EV_USER_CHANGED, NULL);
}

/* 模组全清的应答：face_id 恒为 -1（DELETE_ALL 没有具体模板号）——
 * 单条删除是 page_users 的事，这里只认「全清」。 */
static void on_face_event_sys(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if (topic != EV_FACE_EVENT || payload == NULL) return;
    const ev_face_event_t * e = (const ev_face_event_t *)payload;
    if (e->ev != FACE_EV_DELETE_DONE) return;
    if (e->del.face_id != -1) return;

    if (e->del.err == SAFE_OK) {
        /* 模组侧清空了，本地绑定必须同步清 —— 否则留下一整批「本地有、模组无」
         * 的孤儿，用户刷脸必失败且自己删不掉。 */
        worker_post(clear_face_local_worker, NULL, clear_face_local_done);
        ui_banner("模组人脸已全部清空，本地绑定已同步清除", UI_BANNER_OK, 3000);
    } else {
        ui_banner("清空模组人脸失败，请稍后重试", UI_BANNER_DANGER, 3000);
    }
}

static void clear_face_btn_cb(lv_event_t * e)
{
    (void)e;
    admin_verify_open(ADMIN_ACT_CLEAR_FACE);
}

static void do_clear_face_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
    safe_err_t r = face_service_delete_all_async();
    if (r == SAFE_OK)            ui_banner("正在清空模组人脸…", UI_BANNER_INFO, 2000);
    else if (r == SAFE_ERR_BUSY) ui_banner("模组忙（可能正在录入/识别），请稍后重试",
                                           UI_BANNER_DANGER, 3000);
    else                         ui_banner("当前模组不支持清空操作", UI_BANNER_DANGER, 3000);
}

/* 确认弹窗：版式与 dlg_factory 一致（危险操作双重确认） */
static void dlg_clear_face(void)
{
    close_dlg();
    lv_obj_t * scr = lv_screen_active();
    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_scrollable(s_ov, false);

    const int32_t win_w = 380, win_h = 200;
    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, win_w, win_h);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    lv_obj_center(s_win);
    lv_obj_set_scrollable(s_win, false);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 10, 0);

    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, "将删除模组内的全部人脸模板，且本地所有人脸绑定一并清除");
    lv_obj_add_style(t, &st_danger_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(16), 0);
    lv_obj_set_width(t, win_w - 32);            /* 限宽折行，否则长句冲出窗口 */
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, "此操作不可撤销");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);

    const int32_t btn_y = win_h - 16 - 44 - 16;   /* 用常量算，勿回查 s_win 坐标 */
    const int32_t btn_x = (win_w - 32 - 280) / 2 + 16;

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

    lv_obj_t * yy = ui_icon_text_button(s_win, LV_SYMBOL_TRASH, "确认清空",
                                        136, 44, &st_danger_btn,
                                        lv_color_white(), do_clear_face_cb, NULL);
    lv_obj_set_floating(yy, true);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
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

    /* ★ 尺寸一律用常量算，**不要**在 set_size 之后回查 lv_obj_get_height(s_win)：
     * 刚创建的对象要到下一次布局才有 coords，此处查询恒为 0，会让按钮算到负坐标
     * （page_users.c 的 dlg_bottom_btn_xy 上方有同一个陷阱的注释，那边早就改对了）。 */
    const int32_t win_w = 380, win_h = 200;
    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, win_w, win_h);
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
    /* ★ 必须限宽，否则 LVGL 让标签按内容自适应成**单行**，长句直接冲出窗口右边界
     *   （用户 2026-09-20 截图：「...全部用户 / 网络 / 日」被截断，末尾看不见）。
     *   限宽后自动折行，再居中显示。 */
    lv_obj_set_width(t, win_w - 32);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位（见 page_users 同样注释） */
    const int32_t btn_y = win_h - 16 - 44 - 16;
    const int32_t btn_x = (win_w - 32 - 280) / 2 + 16;

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

    /* 说清「为什么现在要输 PIN」：三个入口（保存策略 / 恢复出厂 / 虚位开关）
     * 共用同一套验证弹窗，不写清动作会让人以为点错了按钮。 */
    const char * why = "请输入管理员 PIN";
    if (act == ADMIN_ACT_SAVE_POLICY)  why = "修改安全策略需管理员验证";
    else if (act == ADMIN_ACT_FACTORY) why = "恢复出厂需管理员验证";
    else if (act == ADMIN_ACT_VIRTUAL_PIN) why = "切换虚位密码需管理员验证";
    else if (act == ADMIN_ACT_CLEAR_FACE)  why = "清空模组人脸需管理员验证";
    else if (act == ADMIN_ACT_ENROLL_MODE) why = "切换人脸录入模式需管理员验证";

    s_av_msg = lv_label_create(s_av_win);
    lv_label_set_text(s_av_msg, why);
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
    case ADMIN_ACT_VIRTUAL_PIN:
        /* 虚位密码开关落盘：后台线程写 users.json，完成后回调刷新控件 */
        vpin_write_async(s_vpin_pending);
        break;
    case ADMIN_ACT_CLEAR_FACE:
        dlg_clear_face();       /* 验证通过后再弹确认框（破坏性操作双重确认） */
        break;
    case ADMIN_ACT_ENROLL_MODE:
        mode_write_async(s_mode_pending);   /* 后台落盘 + 同步后端 */
        break;
    case ADMIN_ACT_NONE:
    default:
        break;
    }
    s_admin_act = ADMIN_ACT_NONE;
}
