/**
 * @file page_network.c
 * 网络（DESIGN.md §7.5）：可用 WiFi（PC 阶段 mock 列表）+ 已存网络（network.json）。
 * 连接：已存直接 mock 连接；未存弹 PSK 输入 → net_add_wifi 落盘。
 */
#include "page_network.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/async_store.h"
#include "core/worker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* PC 阶段 mock 扫描结果 */
static const char * MOCK_SSIDS[] = {
    "HomeWiFi-5G", "100ask_Lab", "ChinaNet-abc123", "guest_free",
};
#define MOCK_N (int)(sizeof(MOCK_SSIDS) / sizeof(MOCK_SSIDS[0]))

static lv_obj_t * s_avail_list;
static lv_obj_t * s_saved_list;
static lv_obj_t * s_status;

static lv_obj_t * s_ov = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_kb  = NULL;
static lv_obj_t * s_msg = NULL;
static char s_cur_ssid[64] = {0};

static void rebuild_saved(void);
static void go_back_cb(lv_event_t * e);
static void wifi_click_cb(lv_event_t * e);
static void net_conn_done(int result, const char * psk);
static void net_add_done(int result);
static void dlg_psk(void);
static void close_dlg(void);
static void kb_focus_cb(lv_event_t * e);
static void psk_save_cb(lv_event_t * e);
static void dlg_cancel_cb(lv_event_t * e);

lv_obj_t * page_network_create(lv_obj_t * parent)
{
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
    lv_label_set_text(title, "网络");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font(28), 0);
    lv_obj_set_style_pad_left(title, 16, 0);

    s_status = lv_label_create(head);
    lv_obj_add_style(s_status, &st_ok_text, 0);
    lv_obj_set_style_text_font(s_status, app_font(14), 0);
    lv_obj_set_style_pad_left(s_status, 20, 0);

    /* 主体：左可用 + 右已存 */
    lv_obj_t * body = lv_obj_create(root);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_size(body, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 16, 0);

    /* 左：可用网络 */
    lv_obj_t * avail = lv_obj_create(body);
    lv_obj_set_flex_grow(avail, 1);
    lv_obj_add_style(avail, &st_panel, 0);
    lv_obj_set_flex_flow(avail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(avail, 8, 0);

    lv_obj_t * at = lv_label_create(avail);
    lv_label_set_text(at, "可用网络（扫描）");
    lv_obj_add_style(at, &st_text, 0);
    lv_obj_set_style_text_font(at, app_font(20), 0);

    s_avail_list = lv_obj_create(avail);
    lv_obj_set_flex_grow(s_avail_list, 1);
    lv_obj_set_size(s_avail_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_avail_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_avail_list, 0, 0);
    lv_obj_set_flex_flow(s_avail_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_avail_list, 6, 0);

    for (int i = 0; i < MOCK_N; i++) {
        lv_obj_t * item = lv_button_create(s_avail_list);
        lv_obj_set_size(item, lv_pct(100), 48);
        lv_obj_add_style(item, &st_panel2, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_add_event_cb(item, wifi_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t * nm = lv_label_create(item);
        lv_label_set_text(nm, MOCK_SSIDS[i]);
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font(16), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t * sig = lv_label_create(item);
        lv_label_set_text(sig, "▂▄▆█");
        lv_obj_add_style(sig, &st_ok_text, 0);
        lv_obj_set_style_text_font(sig, app_font(14), 0);
        lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -14, 0);
    }

    /* 右：已存网络 */
    lv_obj_t * saved = lv_obj_create(body);
    lv_obj_set_flex_grow(saved, 1);
    lv_obj_add_style(saved, &st_panel, 0);
    lv_obj_set_flex_flow(saved, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(saved, 8, 0);

    lv_obj_t * st = lv_label_create(saved);
    lv_label_set_text(st, "已存网络");
    lv_obj_add_style(st, &st_text, 0);
    lv_obj_set_style_text_font(st, app_font(20), 0);

    s_saved_list = lv_obj_create(saved);
    lv_obj_set_flex_grow(s_saved_list, 1);
    lv_obj_set_size(s_saved_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_saved_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_saved_list, 6, 0);

    rebuild_saved();
    return root;
}

static void go_back_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_SETTINGS);
}

/* 读已存网络：4 次 net_get_psk（每次含 PBKDF2 派生）整段在后台执行（DESIGN.md §9） */
typedef struct {
    int count;
    char saved[4][64];
} net_saved_job_t;

static void net_saved_worker(void * p)
{
    net_saved_job_t * a = (net_saved_job_t *)p;
    const char * ssids[] = { "HomeWiFi-5G", "100ask_Lab", "ChinaNet-abc123", "guest_free" };
    int c = 0;
    for (int i = 0; i < 4 && c < 4; i++) {
        char psk[64];
        if (net_get_psk(ssids[i], psk, sizeof(psk)) == 0) {
            strncpy(a->saved[c], ssids[i], sizeof(a->saved[c]) - 1);
            c++;
        }
    }
    a->count = c;
}

static void net_saved_done(void * p)
{
    net_saved_job_t * a = (net_saved_job_t *)p;
    lv_obj_clean(s_saved_list);
    for (int i = 0; i < a->count; i++) {
        lv_obj_t * item = lv_obj_create(s_saved_list);
        lv_obj_set_size(item, lv_pct(100), 44);
        lv_obj_add_style(item, &st_panel2, 0);
        lv_obj_set_style_radius(item, 8, 0);

        lv_obj_t * nm = lv_label_create(item);
        lv_label_set_text(nm, a->saved[i]);
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font(14), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t * ok = lv_label_create(item);
        lv_label_set_text(ok, "已保存");
        lv_obj_add_style(ok, &st_ok_text, 0);
        lv_obj_set_style_text_font(ok, app_font(14), 0);
        lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    if (a->count == 0) {
        lv_obj_t * tip = lv_label_create(s_saved_list);
        lv_label_set_text(tip, "暂无已存网络");
        lv_obj_add_style(tip, &st_text_mut, 0);
        lv_obj_set_style_text_font(tip, app_font(14), 0);
    }
    free(a);
}

static void rebuild_saved(void)
{
    net_saved_job_t * a = (net_saved_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    worker_post(net_saved_worker, a, net_saved_done);
}

/* 已存网络直连：后台查 psk（AES+PBKDF2），回调里决定直连或弹 PSK 输入 */
static void net_conn_done(int result, const char * psk)
{
    (void)psk;
    if (result == 0) {
        astore_append_log("wifi_change", "admin", 1, "connect saved");
        lv_label_set_text(s_status, "已连接");
        rebuild_saved();
    } else {
        dlg_psk();
    }
}

static void wifi_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    strncpy(s_cur_ssid, MOCK_SSIDS[idx], sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';

    astore_net_get_psk(s_cur_ssid, net_conn_done);
}

/* ---------------- PSK 输入弹窗 ---------------- */
static void dlg_psk(void)
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
    lv_obj_align(s_win, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 8, 0);

    char tbuf[80];
    snprintf(tbuf, sizeof(tbuf), "连接 %s", s_cur_ssid);
    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, tbuf);
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font(20), 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font(14), 0);

    lv_obj_t * ta = lv_textarea_create(s_win);
    lv_obj_set_size(ta, 300, 40);
    lv_obj_add_style(ta, &st_panel2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, 32);
    lv_textarea_set_placeholder_text(ta, "WiFi 密码");
    lv_obj_add_event_cb(ta, kb_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位（见 page_users 同样注释） */
    const int32_t btn_y = lv_obj_get_height(s_win) - 16 - 44 - 16;
    const int32_t btn_x = (lv_obj_get_width(s_win) - 32 - 300) / 2 + 16;

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 146, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_add_flag(cc, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * ok = lv_button_create(s_win);
    lv_obj_set_size(ok, 146, 44);
    lv_obj_set_pos(ok, btn_x + 154, btn_y);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, psk_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok);
    lv_label_set_text(okl, "连接");
    lv_obj_set_style_text_font(okl, app_font(14), 0);
    lv_obj_center(okl);

    s_kb = lv_keyboard_create(s_ov);
    lv_obj_set_size(s_kb, lv_pct(100), 210);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, theme_color(TH_PANEL), 0);
    lv_keyboard_set_textarea(s_kb, ta);
}

static void close_dlg(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    s_ov = NULL; s_win = NULL; s_kb = NULL; s_msg = NULL;
}

static void kb_focus_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target(e);
    if (s_kb) lv_keyboard_set_textarea(s_kb, ta);
}

static void dlg_cancel_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

static void psk_save_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * ta = NULL;
    uint32_t cnt = lv_obj_get_child_count(s_win);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t * c = lv_obj_get_child(s_win, i);
        if (lv_obj_check_type(c, &lv_textarea_class)) { ta = c; break; }
    }
    if (!ta) return;
    const char * psk = lv_textarea_get_text(ta);
    if (strlen(psk) < 8) {
        lv_label_set_text(s_msg, "密码至少 8 位");
        return;
    }
    astore_net_add_wifi(s_cur_ssid, "WPA2", psk, net_add_done);
}

static void net_add_done(int result)
{
    if (result == 0) {
        astore_append_log("wifi_change", "admin", 1, s_cur_ssid);
        close_dlg();
        lv_label_set_text(s_status, "已连接");
        rebuild_saved();
    } else {
        lv_label_set_text(s_msg, "保存失败");
    }
}
