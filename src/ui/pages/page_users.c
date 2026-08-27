/**
 * @file page_users.c
 * 用户管理（DESIGN.md §7.3 / §2）：多用户列表 + 添加 + 详情（改密/删除/启用）。
 * 数据经 core/store 落 users.json（PIN 只存 PBKDF2 哈希）。
 * 表单输入统一用 lv_keyboard 绑定当前 textarea（DESIGN.md §7.4）。
 */
#include "page_users.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/async_store.h"
#include "core/worker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static lv_obj_t * s_list;
static lv_obj_t * s_name_lbl;
static lv_obj_t * s_role_lbl;
static lv_obj_t * s_state_lbl;
static lv_obj_t * s_created_lbl;
static int s_sel_id = -1;

/* 弹窗句柄：overlay + 弹窗根 + 共享键盘 + 消息 label */
static lv_obj_t * s_ov = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_kb  = NULL;
static lv_obj_t * s_msg = NULL;

static void rebuild_list(void);
static void refresh_detail(void);
static void user_click_cb(lv_event_t * e);
static void go_back_cb(lv_event_t * e);
static void add_btn_cb(lv_event_t * e);
static void pwd_btn_cb(lv_event_t * e);
static void del_btn_cb(lv_event_t * e);
static void toggle_btn_cb(lv_event_t * e);

static void dlg_add_user(void);
static void dlg_change_pwd(void);
static void dlg_confirm_del(const char *name);
static void dlg_tip(const char *text);
static void close_dlg(void);
static lv_obj_t * dlg_open(const char *title, int w, int h);
static lv_obj_t * dlg_textarea(const char *label, bool password, bool number);
static void kb_focus_cb(lv_event_t * e);
static void save_add_cb(lv_event_t * e);
static void save_pwd_cb(lv_event_t * e);
static void do_del_cb(lv_event_t * e);
static void dlg_cancel_cb(lv_event_t * e);
static void dlg_ok_cb(lv_event_t * e);
static void dlg_set_msg(const char *text);

/* DESIGN.md §9：复合用户操作整段在后台 worker 执行（PBKDF2 + 文件重写），
 * 结果经 worker_poll() 回主线程回调。 */
typedef struct { char name[32]; char pin[16]; int result; } add_user_job_t;
typedef struct { int id; char oldpin[16]; char newpin[16]; int result; } chg_pwd_job_t;
typedef struct { int id; int result; } user_op_job_t;

static void users_list_loaded(safe_user_t * list, int count);
static void detail_loaded(safe_user_t * list, int count);
static void add_user_worker(void * p);
static void add_user_done(void * p);
static void chg_pwd_worker(void * p);
static void chg_pwd_done(void * p);
static void tog_worker(void * p);
static void tog_done(void * p);
static void del_check_loaded(safe_user_t * list, int count);
static void del_worker(void * p);
static void del_done(void * p);

static void salt_to_hex(const uint8_t salt[16], char out[33])
{
    static const char *HEX = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = HEX[salt[i] >> 4];
        out[i * 2 + 1] = HEX[salt[i] & 15];
    }
    out[32] = '\0';
}

lv_obj_t * page_users_create(lv_obj_t * parent)
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
    lv_label_set_text(title, "用户管理");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font(28), 0);
    lv_obj_set_style_pad_left(title, 16, 0);

    lv_obj_t * spacer = lv_obj_create(head);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    lv_obj_t * add = lv_button_create(head);
    lv_obj_set_size(add, 96, 40);
    lv_obj_add_style(add, &st_accent_btn, 0);
    lv_obj_add_style(add, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(add, add_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(add);
    lv_label_set_text(al, "+ 添加");
    lv_obj_set_style_text_font(al, app_font(14), 0);
    lv_obj_center(al);

    /* 主体：左列表 + 右详情 */
    lv_obj_t * body = lv_obj_create(root);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_size(body, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 16, 0);

    s_list = lv_obj_create(body);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_add_style(s_list, &st_panel, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    lv_obj_t * det = lv_obj_create(body);
    lv_obj_set_flex_grow(det, 1);
    lv_obj_add_style(det, &st_panel, 0);
    lv_obj_set_flex_flow(det, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(det, 8, 0);

    lv_obj_t * dt = lv_label_create(det);
    lv_label_set_text(dt, "用户详情");
    lv_obj_add_style(dt, &st_text, 0);
    lv_obj_set_style_text_font(dt, app_font(20), 0);

    s_name_lbl = lv_label_create(det);
    lv_obj_add_style(s_name_lbl, &st_text, 0);
    lv_obj_set_style_text_font(s_name_lbl, app_font(28), 0);

    s_role_lbl = lv_label_create(det);
    lv_obj_add_style(s_role_lbl, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_role_lbl, app_font(14), 0);

    s_state_lbl = lv_label_create(det);
    lv_obj_add_style(s_state_lbl, &st_ok_text, 0);
    lv_obj_set_style_text_font(s_state_lbl, app_font(14), 0);

    s_created_lbl = lv_label_create(det);
    lv_obj_add_style(s_created_lbl, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_created_lbl, app_font(14), 0);

    lv_obj_t * spacer2 = lv_obj_create(det);
    lv_obj_set_flex_grow(spacer2, 1);
    lv_obj_set_style_bg_opa(spacer2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer2, 0, 0);

    lv_obj_t * ops = lv_obj_create(det);
    lv_obj_set_size(ops, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ops, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ops, 0, 0);
    lv_obj_set_flex_flow(ops, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ops, 8, 0);

    lv_obj_t * bp = lv_button_create(ops);
    lv_obj_set_flex_grow(bp, 1);
    lv_obj_set_height(bp, 42);
    lv_obj_add_style(bp, &st_ghost_btn, 0);
    lv_obj_add_event_cb(bp, pwd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bpl = lv_label_create(bp);
    lv_label_set_text(bpl, "改密");
    lv_obj_set_style_text_font(bpl, app_font(14), 0);
    lv_obj_center(bpl);

    lv_obj_t * bt = lv_button_create(ops);
    lv_obj_set_flex_grow(bt, 1);
    lv_obj_set_height(bt, 42);
    lv_obj_add_style(bt, &st_ghost_btn, 0);
    lv_obj_add_event_cb(bt, toggle_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btl = lv_label_create(bt);
    lv_label_set_text(btl, "启用/停用");
    lv_obj_set_style_text_font(btl, app_font(14), 0);
    lv_obj_center(btl);

    lv_obj_t * bd = lv_button_create(ops);
    lv_obj_set_flex_grow(bd, 1);
    lv_obj_set_height(bd, 42);
    lv_obj_add_style(bd, &st_danger_btn, 0);
    lv_obj_add_event_cb(bd, del_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bdl = lv_label_create(bd);
    lv_label_set_text(bdl, "删除");
    lv_obj_set_style_text_font(bdl, app_font(14), 0);
    lv_obj_center(bdl);

    rebuild_list();
    refresh_detail();
    return root;
}

static void go_back_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_SETTINGS);
}

/* ---------------- 列表与详情（异步加载，DESIGN.md §9） ---------------- */
static void users_list_loaded(safe_user_t * us, int n)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t * item = lv_button_create(s_list);
        lv_obj_set_size(item, lv_pct(100), 52);
        lv_obj_add_style(item, &st_panel2, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_add_event_cb(item, user_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)us[i].id);

        lv_obj_t * nm = lv_label_create(item);
        lv_label_set_text(nm, us[i].name);
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font(16), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t * rl = lv_label_create(item);
        lv_label_set_text(rl, strcmp(us[i].role, "admin") == 0 ? "管理员" : "普通用户");
        lv_obj_add_style(rl, &st_text_mut, 0);
        lv_obj_set_style_text_font(rl, app_font(14), 0);
        lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -16, 0);

        if (!us[i].enabled) {
            lv_obj_t * off = lv_label_create(item);
            lv_label_set_text(off, "停用");
            lv_obj_add_style(off, &st_warn_text, 0);
            lv_obj_set_style_text_font(off, app_font(14), 0);
            lv_obj_align(off, LV_ALIGN_RIGHT_MID, -76, 0);
        }
    }
    /* 列表由 astore 框架释放 */
}

static void rebuild_list(void)
{
    astore_load_users(users_list_loaded);
}

static void detail_loaded(safe_user_t * us, int n)
{
    if (s_sel_id < 0) {
        lv_label_set_text(s_name_lbl, "请选择用户");
        lv_label_set_text(s_role_lbl, "");
        lv_label_set_text(s_state_lbl, "");
        lv_label_set_text(s_created_lbl, "");
        return;
    }
    bool found = false;
    safe_user_t u;
    for (int i = 0; i < n; i++) {
        if (us[i].id == s_sel_id) { u = us[i]; found = true; break; }
    }
    if (!found) {
        s_sel_id = -1;
        lv_label_set_text(s_name_lbl, "请选择用户");
        lv_label_set_text(s_role_lbl, "");
        lv_label_set_text(s_state_lbl, "");
        lv_label_set_text(s_created_lbl, "");
        return;
    }

    lv_label_set_text(s_name_lbl, u.name);
    char buf[160];
    snprintf(buf, sizeof(buf), "角色：%s   认证：%s",
             strcmp(u.role, "admin") == 0 ? "管理员" : "普通用户", u.auth_method);
    lv_label_set_text(s_role_lbl, buf);
    lv_obj_add_style(s_state_lbl, u.enabled ? &st_ok_text : &st_warn_text, 0);
    lv_label_set_text(s_state_lbl, u.enabled ? "● 启用中" : "● 已停用");
    snprintf(buf, sizeof(buf), "创建时间：%s    失败次数：%d", u.created_at, u.failed_attempts);
    lv_label_set_text(s_created_lbl, buf);
}

static void refresh_detail(void)
{
    astore_load_users(detail_loaded);
}

static void user_click_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    refresh_detail();
}

/* ---------------- 弹窗基础 ---------------- */
static lv_obj_t * dlg_open(const char *title, int w, int h)
{
    close_dlg();
    lv_obj_t * scr = lv_screen_active();

    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, w, h);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    lv_obj_align(s_win, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 8, 0);

    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, title);
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font(20), 0);

    /* 消息行（校验错误提示） */
    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font(14), 0);

    /* 共享键盘：底部全宽 */
    s_kb = lv_keyboard_create(s_ov);
    lv_obj_set_size(s_kb, lv_pct(100), 210);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, theme_color(TH_PANEL), 0);
    return s_win;
}

static void close_dlg(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    s_ov = NULL; s_win = NULL; s_kb = NULL; s_msg = NULL;
}

static void dlg_set_msg(const char *text)
{
    if (s_msg) lv_label_set_text(s_msg, text);
}

static lv_obj_t * dlg_textarea(const char *label, bool password, bool number)
{
    lv_obj_t * ta = lv_textarea_create(s_win);
    lv_obj_set_size(ta, 300, 40);
    lv_obj_add_style(ta, &st_panel2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, label);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
        lv_textarea_set_max_length(ta, 8);
    }
    lv_obj_add_event_cb(ta, kb_focus_cb, LV_EVENT_FOCUSED, (void *)(uintptr_t)(number ? 1 : 0));
    return ta;
}

static void kb_focus_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target(e);
    bool number = (bool)(uintptr_t)lv_event_get_user_data(e);
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_keyboard_set_mode(s_kb, number ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

static void dlg_cancel_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

static void dlg_ok_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

/* ---------------- 添加用户 ---------------- */
static void add_btn_cb(lv_event_t * e)
{
    (void)e;
    dlg_add_user();
}

static void dlg_add_user(void)
{
    lv_obj_t * win = dlg_open("添加用户", 380, 320);
    (void)win;
    dlg_textarea("用户名（字母数字）", false, false);
    dlg_textarea("PIN（4-8 位数字）", true, true);
    dlg_textarea("确认 PIN", true, true);

    lv_obj_t * save = lv_button_create(s_win);
    lv_obj_set_size(save, 160, 44);
    lv_obj_add_style(save, &st_accent_btn, 0);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");
    lv_obj_set_style_text_font(sl, app_font(16), 0);
    lv_obj_center(sl);
}

static int dlg_get_tas(lv_obj_t * tas[3], int max)
{
    int ti = 0;
    uint32_t cnt = lv_obj_get_child_count(s_win);
    for (uint32_t i = 0; i < cnt && ti < max; i++) {
        lv_obj_t * c = lv_obj_get_child(s_win, i);
        if (lv_obj_check_type(c, &lv_textarea_class)) tas[ti++] = c;
    }
    return ti;
}

/* 添加用户：整段在后台执行（查重/取最大 id/PBKDF2 哈希/落盘/日志） */
static void add_user_worker(void * p)
{
    add_user_job_t * a = (add_user_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    int maxid = 0;
    for (int i = 0; i < n; i++) {
        if (us[i].id > maxid) maxid = us[i].id;
        if (strcmp(us[i].name, a->name) == 0) { user_list_free(us); a->result = -2; return; }
    }
    user_list_free(us);

    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = maxid + 1;
    strncpy(u.name, a->name, sizeof(u.name) - 1);
    strncpy(u.role, "user", sizeof(u.role) - 1);
    strncpy(u.auth_method, "pin", sizeof(u.auth_method) - 1);
    u.enabled = true;
    uint8_t salt[16];
    pin_hash(a->pin, salt, u.pin_hash);
    salt_to_hex(salt, u.pin_salt);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(u.created_at, sizeof(u.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);

    a->result = user_add(&u);
    if (a->result == 0) log_append("user_add", "admin", 1, a->name);
}

static void add_user_done(void * p)
{
    add_user_job_t * a = (add_user_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        rebuild_list();
        refresh_detail();
    } else if (a->result == -2) {
        dlg_set_msg("用户名已存在");
    } else {
        dlg_set_msg("保存失败");
    }
    free(a);
}

static void save_add_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * tas[3];
    int ti = dlg_get_tas(tas, 3);
    if (ti < 3) return;

    const char * name = lv_textarea_get_text(tas[0]);
    const char * pin1 = lv_textarea_get_text(tas[1]);
    const char * pin2 = lv_textarea_get_text(tas[2]);

    const safe_policy_t * pol = user_policy();
    if (!name[0]) { dlg_set_msg("用户名不能为空"); return; }
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) {
            dlg_set_msg("用户名仅限字母数字");
            return;
        }
    }
    size_t plen = strlen(pin1);
    if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
        dlg_set_msg("PIN 长度需 4-8 位");
        return;
    }
    if (strcmp(pin1, pin2) != 0) {
        dlg_set_msg("两次 PIN 不一致");
        return;
    }

    /* 校验通过后把输入副本投递到后台 */
    add_user_job_t * a = (add_user_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->pin, pin1, sizeof(a->pin) - 1);
    worker_post(add_user_worker, a, add_user_done);
}

/* ---------------- 修改密码 ---------------- */
static void pwd_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    dlg_change_pwd();
}

static void dlg_change_pwd(void)
{
    lv_obj_t * win = dlg_open("修改密码", 380, 320);
    (void)win;
    dlg_textarea("旧 PIN", true, true);
    dlg_textarea("新 PIN（4-8 位）", true, true);
    dlg_textarea("确认新 PIN", true, true);

    lv_obj_t * save = lv_button_create(s_win);
    lv_obj_set_size(save, 160, 44);
    lv_obj_add_style(save, &st_accent_btn, 0);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_pwd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");
    lv_obj_set_style_text_font(sl, app_font(16), 0);
    lv_obj_center(sl);
}

/* 改密：整段在后台执行（读用户/验旧 PIN/PBKDF2 新哈希/落盘/日志） */
static void chg_pwd_worker(void * p)
{
    chg_pwd_job_t * a = (chg_pwd_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    safe_user_t u;
    bool found = false;
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { u = us[i]; found = true; break; }
    user_list_free(us);
    if (!found) { a->result = -1; return; }

    if (user_verify_pin(u.name, a->oldpin) != 0) { a->result = 1; return; }  /* 1=旧 PIN 错误 */

    uint8_t salt[16];
    pin_hash(a->newpin, salt, u.pin_hash);
    salt_to_hex(salt, u.pin_salt);
    u.failed_attempts = 0;
    u.lock_until = 0;
    a->result = (user_update(&u) == 0) ? 0 : -1;
    if (a->result == 0) log_append("pwd_change", u.name, 1, "pin updated");
}

static void chg_pwd_done(void * p)
{
    chg_pwd_job_t * a = (chg_pwd_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        refresh_detail();
    } else if (a->result == 1) {
        dlg_set_msg("旧 PIN 错误");
    } else {
        dlg_set_msg("保存失败");
    }
    free(a);
}

static void save_pwd_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * tas[3];
    int ti = dlg_get_tas(tas, 3);
    if (ti < 3) return;

    const char * oldp = lv_textarea_get_text(tas[0]);
    const char * newp = lv_textarea_get_text(tas[1]);
    const char * newp2 = lv_textarea_get_text(tas[2]);

    const safe_policy_t * pol = user_policy();
    size_t plen = strlen(newp);
    if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
        dlg_set_msg("新 PIN 长度需 4-8 位");
        return;
    }
    if (strcmp(newp, newp2) != 0) {
        dlg_set_msg("两次新 PIN 不一致");
        return;
    }

    chg_pwd_job_t * a = (chg_pwd_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = s_sel_id;
    strncpy(a->oldpin, oldp, sizeof(a->oldpin) - 1);
    strncpy(a->newpin, newp, sizeof(a->newpin) - 1);
    worker_post(chg_pwd_worker, a, chg_pwd_done);
}

/* ---------------- 启用/停用 ---------------- */
/* 启用/停用：整段在后台执行 */
static void tog_worker(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    safe_user_t u;
    bool found = false;
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { u = us[i]; found = true; break; }
    user_list_free(us);
    if (!found) { a->result = -1; return; }

    u.enabled = !u.enabled;
    a->result = (user_update(&u) == 0) ? 0 : -1;
    if (a->result == 0) log_append("user_modify", u.name, 1, u.enabled ? "enabled" : "disabled");
}

static void tog_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        rebuild_list();
        refresh_detail();
    }
    free(a);
}

static void toggle_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    user_op_job_t * a = (user_op_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = s_sel_id;
    worker_post(tog_worker, a, tog_done);
}

/* ---------------- 删除 ---------------- */
/* 删除前检查（异步读用户，确认是否允许删 + 取名字） */
static void del_check_loaded(safe_user_t * us, int n)
{
    safe_user_t u;
    bool found = false;
    int admin_count = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].role, "admin") == 0) admin_count++;
        if (us[i].id == s_sel_id) { u = us[i]; found = true; }
    }
    if (!found) return;

    if (strcmp(u.role, "admin") == 0 && admin_count <= 1) {
        dlg_tip("至少保留一名管理员");
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "确定删除用户 %s ？", u.name);
    dlg_confirm_del(msg);
}

static void del_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    astore_load_users(del_check_loaded);
}

static void dlg_confirm_del(const char *msg)
{
    dlg_open("删除确认", 360, 180);
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, msg);
    lv_obj_add_style(m, &st_text, 0);
    lv_obj_set_style_text_font(m, app_font(16), 0);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位。
     * （不要在按钮外再套一层容器——实测会整体偏移；直接挂 s_win 才正常） */
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
    lv_obj_add_event_cb(yy, do_del_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yyl = lv_label_create(yy);
    lv_label_set_text(yyl, "删除");
    lv_obj_set_style_text_font(yyl, app_font(14), 0);
    lv_obj_center(yyl);
}

static void dlg_tip(const char *text)
{
    dlg_open("提示", 340, 150);
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, text);
    lv_obj_add_style(m, &st_warn_text, 0);
    lv_obj_set_style_text_font(m, app_font(16), 0);

    lv_obj_t * okb = lv_button_create(s_win);
    lv_obj_set_size(okb, 120, 40);
    lv_obj_add_style(okb, &st_ghost_btn, 0);
    lv_obj_add_event_cb(okb, dlg_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ok = lv_label_create(okb);
    lv_label_set_text(ok, "知道了");
    lv_obj_set_style_text_font(ok, app_font(14), 0);
    lv_obj_center(ok);
}

/* 删除：整段在后台执行（取名字/落盘/日志） */
static void del_worker(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    char name[32] = {0};
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { strncpy(name, us[i].name, sizeof(name) - 1); break; }
    user_list_free(us);

    a->result = user_del(a->id);
    if (a->result == 0) log_append("user_del", "admin", 1, name);
}

static void del_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        s_sel_id = -1;
        rebuild_list();
        refresh_detail();
    }
    free(a);
}

static void do_del_cb(lv_event_t * e)
{
    (void)e;
    user_op_job_t * a = (user_op_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = s_sel_id;
    worker_post(del_worker, a, del_done);
}
