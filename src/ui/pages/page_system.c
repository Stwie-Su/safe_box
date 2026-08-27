/**
 * @file page_system.c
 * 系统（DESIGN.md §7.6）：时间日期 / 安全策略（失败次数、锁定时长）/ 主题 / 恢复出厂。
 */
#include "page_system.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/async_store.h"
#include "core/worker.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

static lv_obj_t * s_clock_lbl;
static lv_obj_t * s_failed_lbl;
static lv_obj_t * s_lock_lbl;
static lv_obj_t * s_theme_btns[THEME_COUNT];

static lv_obj_t * s_ov = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_msg = NULL;

static void go_back_cb(lv_event_t * e);
static void clock_timer_cb(lv_timer_t * t);
static void policy_dec_cb(lv_event_t * e);
static void policy_inc_cb(lv_event_t * e);
static void save_policy_cb(lv_event_t * e);
static void policy_saved_done(int result);
static void theme_click_cb(lv_event_t * e);
static void factory_worker(void * p);
static void factory_done(void * p);
static void factory_btn_cb(lv_event_t * e);
static void refresh_policy(void);

static void dlg_factory(void);
static void close_dlg(void);
static void dlg_cancel_cb(lv_event_t * e);
static void do_factory_cb(lv_event_t * e);

static int s_pending_max_failed = 5;
static int s_pending_lock_secs  = 60;

/* 简易 stepper 行：label + [-] 值 [+]；which=0 失败次数，1 锁定时长 */
static lv_obj_t * stepper_row(lv_obj_t * parent, const char *name, lv_obj_t **val_lbl,
                              lv_event_cb_t dec_cb, lv_event_cb_t inc_cb, int which)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_add_style(row, &st_panel2, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * nm = lv_label_create(row);
    lv_label_set_text(nm, name);
    lv_obj_add_style(nm, &st_text, 0);
    lv_obj_set_style_text_font(nm, app_font(16), 0);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_set_style_pad_left(nm, 16, 0);

    lv_obj_t * dec = lv_button_create(row);
    lv_obj_set_size(dec, 44, 36);
    lv_obj_add_style(dec, &st_ghost_btn, 0);
    lv_obj_add_event_cb(dec, dec_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)which);
    lv_obj_t * decl = lv_label_create(dec);
    lv_label_set_text(decl, "−");
    lv_obj_set_style_text_font(decl, app_font(20), 0);
    lv_obj_center(decl);

    *val_lbl = lv_label_create(row);
    lv_obj_add_style(*val_lbl, &st_text, 0);
    lv_obj_set_style_text_font(*val_lbl, app_font(16), 0);
    lv_obj_set_style_pad_left(*val_lbl, 14, 0);
    lv_obj_set_style_pad_right(*val_lbl, 14, 0);

    lv_obj_t * inc = lv_button_create(row);
    lv_obj_set_size(inc, 44, 36);
    lv_obj_add_style(inc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(inc, inc_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)which);
    lv_obj_t * incl = lv_label_create(inc);
    lv_label_set_text(incl, "+");
    lv_obj_set_style_text_font(incl, app_font(20), 0);
    lv_obj_center(incl);

    lv_obj_set_style_pad_right(row, 12, 0);
    return row;
}

lv_obj_t * page_system_create(lv_obj_t * parent)
{
    const safe_policy_t * pol = user_policy();
    s_pending_max_failed = pol->max_failed;
    s_pending_lock_secs  = pol->lock_seconds;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 12, 0);

    /* 标题行 */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * back = lv_button_create(head);
    lv_obj_set_size(back, 72, 40);
    lv_obj_add_style(back, &st_ghost_btn, 0);
    lv_obj_add_event_cb(back, go_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, "‹ 返回");
    lv_obj_set_style_text_font(bl, app_font(14), 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "系统");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font(28), 0);
    lv_obj_set_style_pad_left(title, 16, 0);

    s_clock_lbl = lv_label_create(head);
    lv_obj_add_style(s_clock_lbl, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_clock_lbl, app_font(14), 0);
    lv_obj_set_style_pad_left(s_clock_lbl, 20, 0);

    /* 左右分栏 */
    lv_obj_t * body = lv_obj_create(root);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_size(body, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 16, 0);

    /* 左：安全策略 */
    lv_obj_t * left = lv_obj_create(body);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_add_style(left, &st_panel, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);

    lv_obj_t * lt = lv_label_create(left);
    lv_label_set_text(lt, "安全策略");
    lv_obj_add_style(lt, &st_text, 0);
    lv_obj_set_style_text_font(lt, app_font(20), 0);

    stepper_row(left, "连续失败次数上限", &s_failed_lbl, policy_dec_cb, policy_inc_cb, 0);
    stepper_row(left, "锁定时长（秒）",   &s_lock_lbl,   policy_dec_cb, policy_inc_cb, 1);

    lv_obj_t * save = lv_button_create(left);
    lv_obj_set_size(save, 160, 44);
    lv_obj_add_style(save, &st_accent_btn, 0);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_policy_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, "保存策略");
    lv_obj_set_style_text_font(sl, app_font(14), 0);
    lv_obj_center(sl);

    /* 右：主题 + 恢复出厂 */
    lv_obj_t * right = lv_obj_create(body);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_add_style(right, &st_panel, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);

    lv_obj_t * rt = lv_label_create(right);
    lv_label_set_text(rt, "主题");
    lv_obj_add_style(rt, &st_text, 0);
    lv_obj_set_style_text_font(rt, app_font(20), 0);

    for (int i = 0; i < THEME_COUNT; i++) {
        lv_obj_t * b = lv_button_create(right);
        lv_obj_set_size(b, lv_pct(100), 44);
        lv_obj_add_style(b, &st_panel2, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_style(b, &st_accent_btn, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, theme_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t * lb = lv_label_create(b);
        lv_label_set_text(lb, theme_name(i));
        lv_obj_add_style(lb, &st_text, 0);
        lv_obj_set_style_text_font(lb, app_font(16), 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 16, 0);
        s_theme_btns[i] = b;
        if (i == theme_idx()) lv_obj_add_state(b, LV_STATE_CHECKED);
    }

    lv_obj_t * spacer = lv_obj_create(right);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    lv_obj_t * fr = lv_button_create(right);
    lv_obj_set_size(fr, lv_pct(100), 44);
    lv_obj_add_style(fr, &st_danger_btn, 0);
    lv_obj_add_event_cb(fr, factory_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * frl = lv_label_create(fr);
    lv_label_set_text(frl, "恢复出厂设置");
    lv_obj_set_style_text_font(frl, app_font(16), 0);
    lv_obj_center(frl);

    refresh_policy();
    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
    return root;
}

static void go_back_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_SETTINGS);
}

static void clock_timer_cb(lv_timer_t * t)
{
    (void)t;
    char buf[40];
    time_t now = time(NULL);
    struct tm * tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmv);
        lv_label_set_text(s_clock_lbl, buf);
    }
}

static void refresh_policy(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_pending_max_failed);
    lv_label_set_text(s_failed_lbl, buf);
    snprintf(buf, sizeof(buf), "%d", s_pending_lock_secs);
    lv_label_set_text(s_lock_lbl, buf);
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
    /* 策略落盘（重写 users.json）放后台；日志异步追加 */
    astore_set_policy(s_pending_max_failed, s_pending_lock_secs, policy_saved_done);
}

static void policy_saved_done(int result)
{
    (void)result;
    astore_append_log("setting_change", "admin", 1, "policy updated");
}

static void theme_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < THEME_COUNT; i++) lv_obj_remove_state(s_theme_btns[i], LV_STATE_CHECKED);
    lv_obj_add_state(s_theme_btns[idx], LV_STATE_CHECKED);
    theme_switch(idx);
    astore_append_log("setting_change", "admin", 1, theme_name(idx));
}

/* ---------------- 恢复出厂 ---------------- */
static void factory_btn_cb(lv_event_t * e)
{
    (void)e;
    dlg_factory();
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

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, 380, 200);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    lv_obj_center(s_win);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 10, 0);

    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, "恢复出厂将清除全部用户 / 网络 / 日志");
    lv_obj_add_style(t, &st_danger_text, 0);
    lv_obj_set_style_text_font(t, app_font(16), 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font(14), 0);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位（见 page_users 同样注释） */
    const int32_t btn_y = lv_obj_get_height(s_win) - 16 - 44 - 16;
    const int32_t btn_x = (lv_obj_get_width(s_win) - 32 - 280) / 2 + 16;

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 136, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_add_flag(cc, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * yy = lv_button_create(s_win);
    lv_obj_set_size(yy, 136, 44);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
    lv_obj_add_flag(yy, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(yy, &st_danger_btn, 0);
    lv_obj_add_event_cb(yy, do_factory_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yyl = lv_label_create(yy);
    lv_label_set_text(yyl, "确认恢复");
    lv_obj_set_style_text_font(yyl, app_font(14), 0);
    lv_obj_center(yyl);
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
