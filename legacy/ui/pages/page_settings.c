/**
 * @file page_settings.c
 * 设置中枢（DESIGN.md §7.2）：三个入口卡片 + admin PIN 二次验证弹窗。
 * 敏感操作（用户管理 / 网络 / 系统）必须先验证管理员 PIN（DESIGN.md §2.1）。
 */
#include "page_settings.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/store.h"
#include "core/async_store.h"
#include <string.h>

typedef struct {
    ui_page_t target;
    const char * name;
    const char * desc;
    const char * icon;
} entry_t;

static const entry_t ENTRIES[3] = {
    { PAGE_USERS,   "用户管理", "添加 / 删除用户，修改 PIN 与角色", "用" },
    { PAGE_NETWORK, "网络",     "WiFi 连接与已存网络管理",         "网" },
    { PAGE_SYSTEM,  "系统",     "时间 / 安全策略 / 主题 / 恢复出厂", "系" },
};

/* 验证弹窗控件 */
static lv_obj_t * s_overlay;
static lv_obj_t * s_vpin_disp;
static lv_obj_t * s_vmsg;
static char s_vpin[16] = {0};
static ui_page_t s_pending_page = PAGE_SETTINGS;

static void entry_click_cb(lv_event_t * e);
static void show_verify(ui_page_t target);
static void close_verify(void);
static void vkey_cb(lv_event_t * e);
static void vcancel_cb(lv_event_t * e);
static void vok_cb(lv_event_t * e);
static void vupdate_disp(void);

lv_obj_t * page_settings_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_row(root, 16, 0);

    lv_obj_t * title = lv_label_create(root);
    lv_label_set_text(title, "设置");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(28), 0);

    lv_obj_t * sub = lv_label_create(root);
    lv_label_set_text(sub, "以下操作需要管理员验证");
    lv_obj_add_style(sub, &st_text_mut, 0);
    lv_obj_set_style_text_font(sub, app_font_scaled(14), 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t * card = lv_button_create(root);
        lv_obj_set_size(card, lv_pct(100), 96);
        lv_obj_add_style(card, &st_panel, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_add_event_cb(card, entry_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        /* 图标块（装饰容器：必须取消默认可点击，否则会拦截卡片按钮的点击） */
        lv_obj_t * icon = lv_obj_create(card);
        lv_obj_set_clickable(icon, false);
        lv_obj_set_size(icon, 56, 56);
        lv_obj_set_style_bg_color(icon, theme_color(TH_ACCENT), 0);
        lv_obj_set_style_radius(icon, 12, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_t * icl = lv_label_create(icon);
        lv_label_set_text(icl, ENTRIES[i].icon);
        lv_obj_set_style_text_color(icl, theme_color(TH_ACCENT_INK), 0);
        lv_obj_set_style_text_font(icl, app_font_scaled(20), 0);
        lv_obj_center(icl);

        /* 名称 + 描述 */
        lv_obj_t * nm = lv_label_create(card);
        lv_label_set_text(nm, ENTRIES[i].name);
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font_scaled(20), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 92, -14);

        lv_obj_t * ds = lv_label_create(card);
        lv_label_set_text(ds, ENTRIES[i].desc);
        lv_obj_add_style(ds, &st_text_mut, 0);
        lv_obj_set_style_text_font(ds, app_font_scaled(14), 0);
        lv_obj_align(ds, LV_ALIGN_LEFT_MID, 92, 20);

        /* 右箭头 */
        lv_obj_t * ar = lv_label_create(card);
        lv_label_set_text(ar, "›");
        lv_obj_add_style(ar, &st_text_mut, 0);
        lv_obj_set_style_text_font(ar, app_font_scaled(28), 0);
        lv_obj_align(ar, LV_ALIGN_RIGHT_MID, -24, 0);
    }
    return root;
}

static void entry_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    show_verify(ENTRIES[idx].target);
}

/* ---------------- admin PIN 验证弹窗 ---------------- */
static void show_verify(ui_page_t target)
{
    s_pending_page = target;
    lv_obj_t * scr = lv_screen_active();

    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);

    lv_obj_t * win = lv_obj_create(s_overlay);
    lv_obj_set_size(win, 380, 460);
    lv_obj_add_style(win, &st_panel, 0);
    lv_obj_set_style_radius(win, SX(16), 0);
    lv_obj_set_style_pad_all(win, 0, 0);   /* 手动画绝对定位，去掉 st_panel 的 16px padding */
    lv_obj_center(win);

    /* 全部用 lv_obj_set_pos 显式定位：不用 flex/align，避免按钮被压扁/错位 */
    lv_obj_t * t = lv_label_create(win);
    lv_label_set_text(t, "管理员验证");
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    lv_obj_set_width(t, 380);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, 14);

    s_vpin_disp = lv_label_create(win);
    lv_label_set_text(s_vpin_disp, "——");
    lv_obj_add_style(s_vpin_disp, &st_text, 0);
    lv_obj_set_style_text_font(s_vpin_disp, app_font_scaled(28), 0);
    lv_obj_set_style_pad_all(s_vpin_disp, 0, 0);
    lv_obj_set_style_text_align(s_vpin_disp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_vpin_disp, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_vpin_disp, 8, 0);
    lv_obj_set_size(s_vpin_disp, 240, 50);
    lv_obj_set_pos(s_vpin_disp, 70, 54);

    s_vmsg = lv_label_create(win);
    lv_label_set_text(s_vmsg, "请输入管理员 PIN");
    lv_obj_add_style(s_vmsg, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_vmsg, app_font_scaled(14), 0);
    lv_obj_set_width(s_vmsg, 380);
    lv_obj_set_style_text_align(s_vmsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_vmsg, 0, 116);

    /* 键盘 3×4：键直接作为 win 的子对象，绝对坐标（不再套 row 容器）。
     * 键宽 88、列距 96；行距 50。 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    const int ROW_Y[4] = { 160, 210, 260, 310 };
    const int KEY_X[3]  = { 50, 146, 242 };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(win);
            lv_obj_set_size(k, 88, 40);
            lv_obj_set_pos(k, KEY_X[c], ROW_Y[r]);
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, 8, 0);
            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(16), 0);
            lv_obj_center(kl);
            lv_obj_add_event_cb(k, vkey_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)(r * 3 + c));
        }
    }

    /* 取消 / 确认 */
    lv_obj_t * cancel = lv_button_create(win);
    lv_obj_set_size(cancel, 136, 40);
    lv_obj_set_pos(cancel, 50, 360);
    lv_obj_add_style(cancel, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cancel, vcancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, app_font_scaled(16), 0);
    lv_obj_center(cl);

    lv_obj_t * ok = lv_button_create(win);
    lv_obj_set_size(ok, 136, 40);
    lv_obj_set_pos(ok, 194, 360);
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, vok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ol = lv_label_create(ok);
    lv_label_set_text(ol, "确认");
    lv_obj_set_style_text_font(ol, app_font_scaled(16), 0);
    lv_obj_center(ol);

    s_vpin[0] = '\0';
    vupdate_disp();
}

static void close_verify(void)
{
    if (s_overlay) lv_obj_delete(s_overlay);
    s_overlay = NULL;
}

static void vkey_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    static const char * KEYS[12] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "退格", "0", "清空",
    };
    const char * key = KEYS[idx];
    if (strcmp(key, "清空") == 0) {
        s_vpin[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_vpin);
        if (len > 0) s_vpin[len - 1] = '\0';
    } else {
        size_t len = strlen(s_vpin);
        if (len < 8) { s_vpin[len] = key[0]; s_vpin[len + 1] = '\0'; }
    }
    vupdate_disp();
}

static void vupdate_disp(void)
{
    size_t len = strlen(s_vpin);
    char masked[16];
    for (size_t i = 0; i < len; i++) masked[i] = '*';
    masked[len] = '\0';
    lv_label_set_text(s_vpin_disp, len ? masked : "——");
}

static void vcancel_cb(lv_event_t * e)
{
    (void)e;
    close_verify();
}

/* DESIGN.md §9：admin PIN 校验（PBKDF2）投递后台，结果在主线程回调处理。 */
static void vadmin_done(int r)
{
    if (r == 0) {
        astore_append_log("setting_change", "admin", 1, "admin verify ok");
        close_verify();
        ui_switch_page(s_pending_page);
    } else if (r == 2) {
        lv_label_set_text(s_vmsg, "已被锁定，请稍后再试");
        lv_obj_add_style(s_vmsg, &st_warn_text, 0);
        s_vpin[0] = '\0';
        vupdate_disp();
    } else {
        lv_label_set_text(s_vmsg, "PIN 错误，请重试");
        lv_obj_add_style(s_vmsg, &st_warn_text, 0);
        s_vpin[0] = '\0';
        vupdate_disp();
    }
}

static void vok_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_vpin) < 4) {
        lv_label_set_text(s_vmsg, "PIN 至少 4 位");
        lv_obj_add_style(s_vmsg, &st_warn_text, 0);
        return;
    }
    astore_verify_admin(s_vpin, vadmin_done);
}
