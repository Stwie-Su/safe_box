/**
 * @file page_keypad.c
 * 开锁 PIN 键盘（DESIGN.md §7.1 开锁入口）：3×4 数字矩阵 + 掩码输入 + 确认。
 * 校验走 core/unlock_backend（多用户 PIN + 防暴力锁定），结果写审计日志。
 */
#include "page_keypad.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/unlock_backend.h"
#include "core/async_store.h"
#include "core/worker.h"
#include "hal/actuator.h"
#include <string.h>
#include <stdlib.h>

#define PIN_BUF 16

static lv_obj_t * s_display;     /* 输入显示 */
static lv_obj_t * s_msg;         /* 提示 */
static char s_pin[PIN_BUF] = {0};

static void key_click_cb(lv_event_t * e);
static void back_click_cb(lv_event_t * e);
static void confirm_cb(lv_event_t * e);
static void update_display(void);
static void set_msg(const char *text, bool warn);
static void finish_ok(lv_timer_t * t);

lv_obj_t * page_keypad_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);

    /* 整页一律用 lv_obj_set_pos 显式定位（不用 align / flex）。
     * 原因：页面创建后被隐藏、再显示时，lv_obj_align 会按当时的父尺寸重新应用，
     * 导致元素渲染位置偏移；set_pos 是绝对坐标，不受重新布局影响。 */
    int32_t root_w = lv_obj_get_width(root);
    if (root_w <= 0) root_w = lv_obj_get_width(lv_obj_get_parent(root));
    const int32_t kx = (root_w - 300) / 2;   /* 键盘列左缘 */

    /* 标题：占满宽度 + 文本居中 */
    lv_obj_t * title = lv_label_create(root);
    lv_label_set_text(title, "输入 PIN 开锁");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font(28), 0);
    lv_obj_set_width(title, root_w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 14);

    /* 输入显示（掩码）：固定宽居中 */
    s_display = lv_label_create(root);
    lv_label_set_text(s_display, "");
    lv_obj_add_style(s_display, &st_text, 0);
    lv_obj_set_style_text_font(s_display, app_font(28), 0);
    lv_obj_set_style_pad_all(s_display, 8, 0);
    lv_obj_set_style_bg_color(s_display, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_display, 10, 0);
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_display, 240, 54);
    lv_obj_set_pos(s_display, kx + 30, 66);

    /* 提示：占满宽度居中 */
    s_msg = lv_label_create(root);
    lv_label_set_text(s_msg, "PIN 4-8 位，输错 5 次锁定 60 秒");
    lv_obj_add_style(s_msg, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_msg, app_font(14), 0);
    lv_obj_set_width(s_msg, root_w);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_msg, 0, 138);

    /* 键盘 3×4：键直接作为 root 的子对象，绝对坐标（不套 row 容器）。
     * 实测：中间再套一层容器会导致按键整体偏移/被压扁；键直接挂父级 + set_pos 才正常。
     * 键宽 93、列距 100；行距 56；整块 300px 宽以 kx 左缘居中。 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "退格", "0", "清空" },
    };
    const int ROW_Y[4] = { 190, 246, 302, 358 };
    const int KEY_X[3]  = { kx, kx + 100, kx + 200 };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(root);
            lv_obj_set_size(k, 93, 46);
            lv_obj_set_pos(k, KEY_X[c], ROW_Y[r]);
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, 10, 0);

            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font(20), 0);
            lv_obj_center(kl);

            lv_obj_add_event_cb(k, key_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)c);
            lv_obj_set_user_data(k, (void *)(uintptr_t)r);   /* 行号 */
        }
    }

    /* 返回 / 确认：同样直接挂 root */
    lv_obj_t * bk = lv_button_create(root);
    lv_obj_set_size(bk, 145, 46);
    lv_obj_set_pos(bk, kx, 414);
    lv_obj_add_style(bk, &st_ghost_btn, 0);
    lv_obj_add_event_cb(bk, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "返回");
    lv_obj_set_style_text_font(bkl, app_font(16), 0);
    lv_obj_center(bkl);

    lv_obj_t * ok = lv_button_create(root);
    lv_obj_set_size(ok, 145, 46);
    lv_obj_set_pos(ok, kx + 155, 414);
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok);
    lv_label_set_text(okl, "确认");
    lv_obj_set_style_text_font(okl, app_font(16), 0);
    lv_obj_center(okl);

    /* 复位输入 */
    s_pin[0] = '\0';
    update_display();
    set_msg("PIN 4-8 位，输错 5 次锁定 60 秒", false);
    return root;
}

static void key_click_cb(lv_event_t * e)
{
    int col = (int)(uintptr_t)lv_event_get_user_data(e);
    int row = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const char * key = NULL;
    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    key = KEYS[row][col];

    if (strcmp(key, "清空") == 0) {
        s_pin[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_pin);
        if (len > 0) s_pin[len - 1] = '\0';
    } else {
        size_t len = strlen(s_pin);
        if (len < 8) { s_pin[len] = key[0]; s_pin[len + 1] = '\0'; }
    }
    update_display();
}

static void update_display(void)
{
    size_t len = strlen(s_pin);
    char masked[32];
    for (size_t i = 0; i < len; i++) masked[i] = '*';
    masked[len] = '\0';
    lv_label_set_text(s_display, len ? masked : "——");
}

static void set_msg(const char *text, bool warn)
{
    lv_label_set_text(s_msg, text);
    lv_obj_remove_style(s_msg, &st_text_mut, 0);
    lv_obj_remove_style(s_msg, &st_warn_text, 0);
    lv_obj_remove_style(s_msg, &st_ok_text, 0);
    lv_obj_add_style(s_msg, warn ? &st_warn_text : &st_text_mut, 0);
}

static void back_click_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_HOME);
}

/* DESIGN.md §9：开锁校验（对每个启用用户做 PBKDF2）放到后台 worker，
 * UI 不冻结；按钮用 busy 标志防重复提交。 */
typedef struct {
    char pin[16];
    int  result;          /* unlock_result_t */
    char user[32];
} unlock_job_t;

static bool s_verify_busy = false;

static void unlock_worker(void * p)
{
    unlock_job_t * a = (unlock_job_t *)p;
    a->user[0] = '\0';
    a->result = (int)backend_verify_pin(a->pin, a->user, sizeof(a->user));
}

static void unlock_done(void * p)
{
    unlock_job_t * a = (unlock_job_t *)p;
    s_verify_busy = false;

    switch (a->result) {
        case UNLOCK_OK:
            astore_append_log("unlock", a->user[0] ? a->user : "-", 1, "pin ok");
            actuator_drive(true);                 /* PC mock：置开锁状态 */
            set_msg("开锁成功", false);
            lv_label_set_text(s_display, "✓");
            s_pin[0] = '\0';
            lv_timer_t * tm = lv_timer_create(finish_ok, 1200, NULL);
            lv_timer_set_repeat_count(tm, 1);
            break;
        case UNLOCK_LOCKED:
            astore_append_log("unlock_fail", "-", 0, "locked");
            set_msg("已被锁定，请稍后再试", true);
            s_pin[0] = '\0';
            update_display();
            break;
        default:
            astore_append_log("unlock_fail", "-", 0, "pin wrong");
            set_msg("PIN 错误，请重试", true);
            s_pin[0] = '\0';
            update_display();
            break;
    }
    free(a);
}

static void confirm_cb(lv_event_t * e)
{
    (void)e;
    size_t len = strlen(s_pin);
    if (len < 4) {
        set_msg("PIN 至少 4 位", true);
        return;
    }
    if (s_verify_busy) return;   /* 防重复提交 */

    s_verify_busy = true;
    unlock_job_t * a = (unlock_job_t *)calloc(1, sizeof(*a));
    if (!a) { s_verify_busy = false; return; }
    strncpy(a->pin, s_pin, sizeof(a->pin) - 1);
    worker_post(unlock_worker, a, unlock_done);
}

/* 开锁成功后回主页（一次性定时器回调） */
static void finish_ok(lv_timer_t * t)
{
    (void)t;
    ui_switch_page(PAGE_HOME);
}
