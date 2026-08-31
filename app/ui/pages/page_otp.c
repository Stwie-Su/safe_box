/**
 * @file page_otp.c
 * 动态密码页：6 位 TOTP 输入 + 30s 窗口倒计时 + 确认/返回。
 *
 * 与 page_keypad 同款绝对定位键盘（避免重排偏移），输入掩码显示，
 * 确认时调用 auth_fsm_submit_otp()；FSM 通过 UI hook 负责切页（成功回主页、
 * 失败留在当前页并提示）。本页不持有任何判定逻辑（NFR-5）。
 */
#include "page_otp.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/auth/auth_fsm.h"
#include "core/auth/totp.h"
#include "hal/hal_time.h"
#include <string.h>
#include <stdio.h>

#define OTP_BUF 8

static lv_obj_t * s_display;     /* 输入显示（掩码） */
static lv_obj_t * s_msg;         /* 提示 */
static lv_obj_t * s_user;        /* 待验证用户 */
static lv_obj_t * s_countdown;   /* 30s 窗口倒计时 */
static char s_code[OTP_BUF] = {0};

static void otp_key_cb(lv_event_t * e);
static void otp_confirm_cb(lv_event_t * e);
static void otp_back_cb(lv_event_t * e);
static void otp_timer_cb(lv_timer_t * t);
static void update_display(void);
static void set_msg(const char *text, bool warn);

lv_obj_t * page_otp_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);

    lv_display_t * disp = lv_display_get_default();
    const int32_t screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 1024;
    const int32_t kx = (screen_w - SX(300)) / 2;

    lv_obj_t * title = lv_label_create(root);
    lv_label_set_text(title, "输入动态密码");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(28), 0);
    lv_obj_set_width(title, screen_w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, SY(14));

    /* 待验证用户 */
    s_user = lv_label_create(root);
    lv_label_set_text(s_user, "用户：--");
    lv_obj_add_style(s_user, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_user, app_font_scaled(15), 0);
    lv_obj_set_width(s_user, screen_w);
    lv_obj_set_style_text_align(s_user, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_user, 0, SY(54));

    /* 输入显示 */
    s_display = lv_label_create(root);
    lv_label_set_text(s_display, "------");
    lv_obj_add_style(s_display, &st_text, 0);
    lv_obj_set_style_text_font(s_display, app_font_scaled(34), 0);
    lv_obj_set_style_pad_all(s_display, SX(8), 0);
    lv_obj_set_style_bg_color(s_display, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_display, SX(10), 0);
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_display, SX(240), SY(54));
    lv_obj_set_pos(s_display, kx + SX(30), SY(90));

    /* 提示 */
    s_msg = lv_label_create(root);
    lv_label_set_text(s_msg, "人脸已通过，请输入动态密码");
    lv_obj_add_style(s_msg, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);
    lv_obj_set_width(s_msg, screen_w);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_msg, 0, SY(156));

    /* 30s 窗口倒计时 */
    s_countdown = lv_label_create(root);
    lv_label_set_text(s_countdown, "动态码剩余 30s");
    lv_obj_add_style(s_countdown, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_countdown, app_font_scaled(13), 0);
    lv_obj_set_width(s_countdown, screen_w);
    lv_obj_set_style_text_align(s_countdown, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_countdown, 0, SY(184));

    /* 键盘 3×4 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "退格", "0", "清空" },
    };
    const int ROW_Y[4] = { SY(216), SY(272), SY(328), SY(384) };
    const int KEY_X[3]  = { kx, kx + SX(100), kx + SX(200) };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(root);
            lv_obj_set_size(k, SX(93), SY(46));
            lv_obj_set_pos(k, KEY_X[c], ROW_Y[r]);
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, SX(10), 0);
            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(20), 0);
            lv_obj_center(kl);
            lv_obj_add_event_cb(k, otp_key_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)c);
            lv_obj_set_user_data(k, (void *)(uintptr_t)r);
        }
    }

    /* 返回 / 确认 */
    lv_obj_t * bk = lv_button_create(root);
    lv_obj_set_size(bk, SX(145), SY(46));
    lv_obj_set_pos(bk, kx, SY(440));
    lv_obj_add_style(bk, &st_ghost_btn, 0);
    lv_obj_add_event_cb(bk, otp_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "返回");
    lv_obj_set_style_text_font(bkl, app_font_scaled(16), 0);
    lv_obj_center(bkl);

    lv_obj_t * ok = lv_button_create(root);
    lv_obj_set_size(ok, SX(145), SY(46));
    lv_obj_set_pos(ok, kx + SX(155), SY(440));
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, otp_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok);
    lv_label_set_text(okl, "确认");
    lv_obj_set_style_text_font(okl, app_font_scaled(16), 0);
    lv_obj_center(okl);

    s_code[0] = '\0';
    update_display();

    /* 进入本页时同步一次待验证用户与倒计时 */
    const char * pu = auth_fsm_pending_user();
    lv_label_set_text(s_user, pu && *pu ? pu : "用户：--");
    otp_timer_cb(NULL);

    lv_timer_create(otp_timer_cb, 1000, NULL);
    return root;
}

static void otp_key_cb(lv_event_t * e)
{
    int col = (int)(uintptr_t)lv_event_get_user_data(e);
    int row = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    const char * key = KEYS[row][col];

    if (strcmp(key, "清空") == 0) {
        s_code[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_code);
        if (len > 0) s_code[len - 1] = '\0';
    } else {
        size_t len = strlen(s_code);
        if (len < 6) { s_code[len] = key[0]; s_code[len + 1] = '\0'; }
    }
    update_display();
}

static void update_display(void)
{
    size_t len = strlen(s_code);
    char masked[16];
    for (size_t i = 0; i < 6; i++) masked[i] = (i < len) ? '*' : '-';
    masked[6] = '\0';
    lv_label_set_text(s_display, masked);
}

static void set_msg(const char *text, bool warn)
{
    lv_label_set_text(s_msg, text);
    lv_obj_remove_style(s_msg, &st_text_mut, 0);
    lv_obj_remove_style(s_msg, &st_warn_text, 0);
    lv_obj_remove_style(s_msg, &st_ok_text, 0);
    lv_obj_add_style(s_msg, warn ? &st_warn_text : &st_text_mut, 0);
}

static void otp_back_cb(lv_event_t * e)
{
    (void)e;
    auth_fsm_cancel_otp();
    ui_switch_page(PAGE_HOME);
}

static void otp_confirm_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_code) != 6) {
        set_msg("请输入 6 位动态密码", true);
        return;
    }
    auth_fsm_submit_otp(s_code);
    s_code[0] = '\0';
    update_display();
    /* 同步反馈：FSM 已同步处理。成功 -> hook 切回主页；失败 -> 留在本页提示。 */
    if (auth_fsm_state() == FSM_DENY) {
        set_msg("动态密码错误，请重试", true);
    } else if (auth_fsm_state() == FSM_LOCKOUT) {
        set_msg("设备已锁定，请稍后再试", true);
    } else {
        set_msg("验证通过，正在开锁", false);
    }
}

static void otp_timer_cb(lv_timer_t * t)
{
    (void)t;
    /* TOTP 30s 窗口剩余秒数 */
    int rem = 30 - (int)(hal_time() % 30);
    if (rem <= 0) rem = 30;
    char buf[32];
    snprintf(buf, sizeof(buf), "动态码剩余 %ds", rem);
    lv_label_set_text(s_countdown, buf);
}
