/**
 * @file page_keypad.c
 * 开锁 PIN 键盘（DESIGN.md §7.1 开锁入口）：3×4 数字矩阵 + 掩码输入 + 确认。
 * 校验走 core/unlock_backend（多用户 PIN + 防暴力锁定），结果写审计日志。
 */
#include "page_keypad.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/auth/auth_fsm.h"
#include "core/store/store.h"          /* user_policy()：输入上限与提示文案取真实策略 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* FR-18 虚位密码：开启时最长可输入 12 位（v1.9 由 20 收紧；正确 PIN 前后可加干扰位）。 */
#define PIN_BUF (SAFE_VIRTUAL_PIN_MAX_INPUT + 1)

static lv_obj_t * s_display;     /* 输入显示 */
static lv_obj_t * s_msg;         /* 提示 */
static char s_pin[PIN_BUF] = {0};

static void key_click_cb(lv_event_t * e);
static void back_click_cb(lv_event_t * e);
static void confirm_cb(lv_event_t * e);
static void update_display(void);
static void set_msg(const char *text, bool warn);
static int  keypad_max_input(void);
static void refresh_hint(void);
/* 注：已删除 finish_ok(lv_timer_t *) 的未定义声明——全仓无定义、无调用
 * （PIN 校验走 worker 异步回调，不走定时器），属半截功能残留（D11）。 */

lv_obj_t * page_keypad_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);

    /* 整页一律用 lv_obj_set_pos 显式定位（不用 align / flex）。
     * 原因：页面创建后被隐藏、再显示时，lv_obj_align 会按当时的父尺寸重新应用，
     * 导致元素渲染位置偏移；set_pos 是绝对坐标，不受重新布局影响。
     *
     * ★ 修复：取屏幕水平分辨率（而非 lv_obj_get_width）计算 kx。
     * 原因：root 一创建完 width 还是 lv_pct(100) 占位，lv_obj_get_width 返回 0；
     * 退到 parent(s_content) 在 page 还没排版时也不可靠，实测算成 0，
     * 导致 kx 变成 -150，整块键盘被推到屏幕左侧。直接用屏幕分辨率最稳。 */
    lv_display_t * disp = lv_display_get_default();
    const int32_t screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 1024;
    const int32_t kx = (screen_w - SX(300)) / 2;   /* 键盘列左缘 */

    /* 标题：占满宽度 + 文本居中 */
    lv_obj_t * title = lv_label_create(root);
    lv_label_set_text(title, "输入 PIN 开锁");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(28), 0);
    lv_obj_set_width(title, screen_w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, SY(14));

    /* 输入显示（掩码）：固定宽居中 */
    s_display = lv_label_create(root);
    lv_label_set_text(s_display, "");
    lv_obj_add_style(s_display, &st_text, 0);
    lv_obj_set_style_text_font(s_display, app_font_scaled(28), 0);
    lv_obj_set_style_pad_all(s_display, SX(8), 0);
    lv_obj_set_style_bg_color(s_display, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_display, SX(10), 0);
    lv_obj_set_style_text_align(s_display, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_display, SX(240), SY(54));
    lv_obj_set_pos(s_display, kx + SX(30), SY(66));

    /* 提示：占满宽度居中 */
    s_msg = lv_label_create(root);
    lv_label_set_text(s_msg, "PIN 4-8 位，输错 5 次锁定 60 秒");
    lv_obj_add_style(s_msg, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);
    lv_obj_set_width(s_msg, screen_w);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_msg, 0, SY(138));

    /* 键盘 3×4：键直接作为 root 的子对象，绝对坐标（不套 row 容器）。
     * 实测：中间再套一层容器会导致按键整体偏移/被压扁；键直接挂父级 + set_pos 才正常。
     * 键宽 93、列距 100；行距 56；整块 300px 宽以 kx 左缘居中。 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "退格", "0", "清空" },
    };
    const int ROW_Y[4] = { SY(190), SY(250), SY(310), SY(370) };
    const int KEY_X[3]  = { kx, kx + SX(100), kx + SX(200) };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(root);
            lv_obj_set_size(k, SX(93), SY(52));
            lv_obj_set_pos(k, KEY_X[c], ROW_Y[r]);
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, SX(10), 0);

            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(20), 0);
            lv_obj_center(kl);

            lv_obj_add_event_cb(k, key_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)c);
            lv_obj_set_user_data(k, (void *)(uintptr_t)r);   /* 行号 */
        }
    }

    /* 返回 / 确认：同样直接挂 root */
    lv_obj_t * bk = lv_button_create(root);
    lv_obj_set_size(bk, SX(145), SY(52));
    lv_obj_set_pos(bk, kx, SY(430));
    lv_obj_add_style(bk, &st_ghost_btn, 0);
    lv_obj_add_event_cb(bk, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bkl = lv_label_create(bk);
    lv_label_set_text(bkl, "返回");
    lv_obj_set_style_text_font(bkl, app_font_scaled(16), 0);
    lv_obj_center(bkl);

    lv_obj_t * ok = lv_button_create(root);
    lv_obj_set_size(ok, SX(145), SY(52));
    lv_obj_set_pos(ok, kx + SX(155), SY(430));
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok);
    lv_label_set_text(okl, "确认");
    lv_obj_set_style_text_font(okl, app_font_scaled(16), 0);
    lv_obj_center(okl);

    /* 复位输入 */
    s_pin[0] = '\0';
    update_display();
    refresh_hint();     /* 提示文案取自真实策略（含虚位是否开启），不再写死 4-8 位 */
    return root;
}

/* 输入上限：虚位开启 → 12 位（规约 §5.2，v1.9）；否则 → 策略 pin_max_len。
 * 上限本身也受 PIN 合法长度约束：若管理员把 pin_max_len 调得比 12 还大，
 * 以 pin_max_len 为准（此时「虚位」已无意义，但不应反而收窄输入）。
 * 最终再夹到缓冲容量内（PIN_BUF-1）：pin_max_len 无上层设置入口、仅 users.json 可改，
 * 但无论如何都不能让 s_pin[len+1] 越界写。 */
static int keypad_max_input(void)
{
    const safe_policy_t *pol = user_policy();
    int mx = (pol->pin_max_len > 0) ? pol->pin_max_len : 8;
    if (pol->virtual_pin_enable && mx < SAFE_VIRTUAL_PIN_MAX_INPUT)
        mx = SAFE_VIRTUAL_PIN_MAX_INPUT;
    if (mx > PIN_BUF - 1) mx = PIN_BUF - 1;
    return mx;
}

/* 提示文案：长度范围、是否支持虚位、失败锁定阈值，全部读 user_policy()，
 * 避免策略改了界面还在说旧数字（原来这里写死「4-8 位 / 5 次 / 60 秒」）。 */
static void refresh_hint(void)
{
    const safe_policy_t *pol = user_policy();
    char buf[192];
    if (pol->virtual_pin_enable) {
        snprintf(buf, sizeof(buf),
                 "PIN %d-%d 位（可前后加干扰位，最长 %d 位），输错 %d 次锁定 %d 秒",
                 pol->pin_min_len, pol->pin_max_len, SAFE_VIRTUAL_PIN_MAX_INPUT,
                 pol->max_failed, pol->lock_seconds);
    } else {
        snprintf(buf, sizeof(buf), "PIN %d-%d 位，输错 %d 次锁定 %d 秒",
                 pol->pin_min_len, pol->pin_max_len, pol->max_failed, pol->lock_seconds);
    }
    set_msg(buf, false);
}

static void key_click_cb(lv_event_t * e)
{
    /* 后台校验期间冻结输入：避免在「校验中」时改动缓冲，造成结果与显示错位。 */
    if (auth_fsm_state() == FSM_VERIFYING) return;

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
        /* FR-18：上限不再是写死的 8 位 —— 虚位开启时可输入到 12 位（v1.9）。
         * 超过 12 位的输入按规约 §5.2「超过 12 位不受理」直接丢弃，不发往校验层。 */
        if (len < (size_t)keypad_max_input()) { s_pin[len] = key[0]; s_pin[len + 1] = '\0'; }
    }
    update_display();
}

static void update_display(void)
{
    size_t len = strlen(s_pin);
    char masked[PIN_BUF];
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

/* 阶段 1：PIN 校验统一走 auth_fsm（FSM 负责判定 + 执行器脉冲 + 审计日志 + MQTT 事件，
 * 避免 PIN / 人脸 / 动态码三套逻辑重复）。
 * v1.9：判定已异步化——本函数只负责「长度预检 + 提交 + 显示校验中」，
 * 结果由 core 经 ui.c 的 fsm_ui_hook 回填（page_keypad_on_fsm），主线程不再阻塞。 */
static void confirm_cb(lv_event_t * e)
{
    (void)e;
    size_t len = strlen(s_pin);
    const safe_policy_t *pol = user_policy();
    if (len < (size_t)pol->pin_min_len) {
        char buf[64];
        snprintf(buf, sizeof(buf), "PIN 至少 %d 位", pol->pin_min_len);
        set_msg(buf, true);
        return;
    }
    if (auth_fsm_state() == FSM_VERIFYING) return;   /* 已在校验：忽略重复确认 */

    /* 提交即进入校验中（异步）：立即清空缓冲并给出「校验中」提示；
     * 成功由 fsm_ui_hook 切回主页，失败/锁定由 page_keypad_on_fsm 就地回填。 */
    set_msg("正在校验，请稍候", false);
    auth_fsm_submit_pin(s_pin);
    s_pin[0] = '\0';
    update_display();
}

/* PIN 异步校验结果 → 键盘页提示（v1.9）：由 ui.c 的 fsm_ui_hook 在主线程调用，
 * 仅在键盘页当前可见时触发。成功（UNLOCKED）路径已切页，无需在此处理。 */
void page_keypad_on_fsm(fsm_state_t st, const char *detail)
{
    (void)detail;
    switch (st) {
        case FSM_VERIFYING: set_msg("正在校验，请稍候", false);     break;
        case FSM_DENY:      set_msg("PIN 错误，请重试", true);        break;
        case FSM_LOCKOUT:   set_msg("设备已锁定，请稍后再试", true);  break;
        default:            break;
    }
}


