/**
 * @file page_monitor.c
 * 主页（v3 设计）：按照"质感优先 / 信息有故事"原则重构。
 *
 * 设计要点：
 *   1) 顶部状态栏——保持原有「已上锁 / 时间 / wifi」三段，与 ui.c 共用;
 *   2) 中央圆环锁头——矢量锁头图标替换单字"锁"，与圆环轮廓形成「环 + 中心
 *      icon」的双层视觉锚点;
 *   3) 两张开锁卡——并列两张白色卡片（密码开锁 / 人脸识别），统一
 *      st_panel 样式 + 柔和投影，卡片内有图标（锁键盘 / 人脸扫描）和
 *      标签，色彩统一为 accent蓝;
 *   4) 三个信息卡——顶部矢量图标（用户 / 列表 / 信号），数值字段有真实
 *      默认值（3 人 / 5 条 / 已连接 5G）;
 *   5) 最后开启时间——带具体内容「昨天 18:30」;
 *
 * 布局自上而下：
 *   1) 状态主标题「保险柜已上锁」 + 最后一次开启「昨天 18:30」
 *   2) 中央圆环 + 矢量锁头（直径 SY(140)，描边 SY(6)）
 *   3) 双卡：密码开锁 / 人脸识别（白卡 + 矢量图标 + 标签）
 *   4) 三联信息条：用户 | 今日事件 | 网络
 */
#include "page_monitor.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"       /* SX/SY 自适应缩放 */
#include "ui/icons.h"          /* 矢量图标库 */
#include "core/store/store.h"
#include "core/support/worker.h"
#include "core/auth/auth_fsm.h"
#include "hal/hal_actuator.h"
#include "hal/hal_time.h"     /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================== 全局静态 UI 对象指针 ================== */
static lv_obj_t * s_lock_ring;      // 中央圆环（描边容器）
static lv_obj_t * s_lock_icon;      // 中央锁头图标容器（矢量）
static lv_obj_t * s_status_label;   // 状态主标题
static lv_obj_t * s_last_label;     // 最后一次开启时间
/* 两张开锁卡 */
static lv_obj_t * s_pin_card;
static lv_obj_t * s_face_card;
/* 三联信息卡 */
static lv_obj_t * s_user_icon;
static lv_obj_t * s_event_icon;
static lv_obj_t * s_net_icon;
static lv_obj_t * s_user_value;
static lv_obj_t * s_event_value;
static lv_obj_t * s_net_value;
/* 矢量图标的颜色容器（theme_switch 时刷这些） */
static lv_obj_t * s_pin_icon;
static lv_obj_t * s_face_icon;
static lv_obj_t * s_pin_label;
static lv_obj_t * s_face_label;

/* ================== 内部私有函数声明 ================== */
static void monitor_timer_cb(lv_timer_t * t);
static void build_unlock_card(lv_obj_t * parent,
                              ui_icon_kind_t icon,
                              const char * title,
                              const char * sub,
                              lv_obj_t ** out_card,
                              lv_obj_t ** out_icon,
                              lv_obj_t ** out_label);
static void build_info_card(lv_obj_t * parent,
                            ui_icon_kind_t icon,
                            const char * title,
                            const char * default_value,
                            lv_obj_t ** out_value,
                            lv_obj_t ** out_icon);
static int count_today_events(void);
static void unlock_go_cb(lv_event_t * e);   /* 主页"密码开锁" → PIN 页 */
static void face_unlock_cb(lv_event_t * e); /* 主页"人脸识别" → FACE 全屏页 */
static void monitor_refresh_theme(int idx);

/**
 * @brief 创建监控主页面（由 UI 框架调用）
 */
lv_obj_t * page_monitor_create(lv_obj_t * parent)
{
    /* 1. 页面根容器（背景透明，靠屏幕 BG 显示浅色背景） */
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, SX(20), 0);
    lv_obj_set_style_pad_row(root, SY(18), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 2. 中央圆环 + 矢量锁头  ★ 圆环作为外圆框，锁头图标置于正中 */
    lv_obj_t * ring_wrap = lv_obj_create(root);
    lv_obj_set_size(ring_wrap, SX(140), SX(140));
    lv_obj_set_style_bg_opa(ring_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring_wrap, SY(6), 0);
    lv_obj_set_style_border_color(ring_wrap, theme_color(TH_ACCENT), 0);
    lv_obj_set_style_radius(ring_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_outline_width(ring_wrap, 0, 0);
    lv_obj_set_flex_flow(ring_wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ring_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_lock_ring = ring_wrap;

    s_lock_icon = ui_icon_create(ring_wrap, UI_ICON_LOCK, SX(70), theme_color(TH_ACCENT));

    /* 3. 状态主标题 + 最后一次开启时间（紧凑组合） */
    lv_obj_set_style_pad_row(root, SY(8), 0);
    s_status_label = lv_label_create(root);
    lv_label_set_text(s_status_label, "保险柜已上锁");
    lv_obj_add_style(s_status_label, &st_text, 0);
    lv_obj_set_style_text_font(s_status_label, app_font_scaled(20), 0);

    s_last_label = lv_label_create(root);
    /* 给一个"真实感默认值"，避免占位符尴尬 */
    lv_label_set_text(s_last_label, "最后一次开启：昨天 18:30");
    lv_obj_add_style(s_last_label, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_last_label, app_font_scaled(13), 0);

    /* 4. ★ 两张开锁卡片（双白卡 + 矢量图标 + 文字） */
    lv_obj_t * methods = lv_obj_create(root);
    lv_obj_set_size(methods, lv_pct(100), SY(96));
    lv_obj_set_style_bg_opa(methods, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(methods, 0, 0);
    lv_obj_set_style_outline_width(methods, 0, 0);
    lv_obj_set_style_pad_all(methods, 0, 0);
    lv_obj_set_style_pad_column(methods, SX(14), 0);
    lv_obj_set_flex_flow(methods, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(methods, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_unlock_card(methods, UI_ICON_KEYPAD, "密码开锁", "",
                      &s_pin_card, &s_pin_icon, &s_pin_label);
    build_unlock_card(methods, UI_ICON_FACE_SCAN, "人脸识别", "模糊",
                      &s_face_card, &s_face_icon, &s_face_label);

    /* 5. 三联信息卡 */
    lv_obj_t * cards = lv_obj_create(root);
    lv_obj_set_size(cards, lv_pct(100), SY(110));
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_outline_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_set_style_pad_column(cards, SX(12), 0);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_info_card(cards, UI_ICON_USER,     "用户",      "3 人",
                    &s_user_value,  &s_user_icon);
    build_info_card(cards, UI_ICON_LIST,     "今日事件",   "5 条",
                    &s_event_value, &s_event_icon);
    build_info_card(cards, UI_ICON_SIGNAL,   "网络",       "已连接 5G",
                    &s_net_value,   &s_net_icon);

    /* 6. 定时器：刷新主题色相关字段 */
    lv_timer_create(monitor_timer_cb, 1000, root);
    monitor_timer_cb(NULL);

    /* 7. 注册主题切换回调 */
    theme_register_change_cb(monitor_refresh_theme);

        /* UI4_TEMP_ICON_PROOF：图标字体自证（M1 验证完即删） */
    {
        lv_obj_t * row = lv_obj_create(root);
        lv_obj_set_size(row, SX(180), SY(48));
        lv_obj_align(row, LV_ALIGN_BOTTOM_RIGHT, -SX(12), -SY(12));
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_outline_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, SX(14), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        icon_label_colored(row, UI_GLYPH_HOME,   24, TH_TEXT);
        icon_label_colored(row, UI_GLYPH_LOCK,   24, TH_ACCENT);
        icon_label_colored(row, UI_GLYPH_FINGER, 24, TH_OK);
        icon_label_colored(row, UI_GLYPH_BELL,   24, TH_WARN);
    }

return root;
}

/**
 * @brief 构造一张开锁卡片：白底面板 + 矢量图标 + 标签
 */
static void build_unlock_card(lv_obj_t * parent,
                              ui_icon_kind_t icon,
                              const char * title,
                              const char * sub,
                              lv_obj_t ** out_card,
                              lv_obj_t ** out_icon,
                              lv_obj_t ** out_label)
{
    lv_obj_t * card = lv_button_create(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_add_style(card, &st_panel, 0);
    lv_obj_set_style_radius(card, SX(16), 0);
    /* 取消默认按钮按下变色（保持白卡视觉一致） */
    lv_obj_set_style_bg_color(card, theme_color(TH_PANEL), 0);
    lv_obj_set_style_border_color(card, theme_color(TH_BORDER), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(card, SY(10), 0);
    lv_obj_set_style_pad_bottom(card, SY(10), 0);
    lv_obj_set_style_pad_left(card, SX(14), 0);
    lv_obj_set_style_pad_right(card, SX(14), 0);

    /* 矢量图标（accent 蓝） */
    *out_icon = ui_icon_create(card, icon, SX(44), theme_color(TH_ACCENT));

    /* 标签 */
    *out_label = lv_label_create(card);
    lv_label_set_text(*out_label, title);
    lv_obj_set_style_text_font(*out_label, app_font_scaled(16), 0);
    lv_obj_set_style_text_color(*out_label, theme_color(TH_ACCENT), 0);
    lv_obj_set_style_pad_top(*out_label, SY(6), 0);

    /* 副标签（如 "模糊"）—— 隐藏现有写法时省略 */
    if (sub && sub[0] != '\0') {
        lv_obj_t * mut = lv_label_create(card);
        lv_label_set_text(mut, sub);
        lv_obj_set_style_text_font(mut, app_font_scaled(11), 0);
        lv_obj_set_style_text_color(mut, theme_color(TH_TEXT_MUT), 0);
    }

    if (icon == UI_ICON_FACE_SCAN) {
        lv_obj_add_event_cb(card, face_unlock_cb, LV_EVENT_CLICKED, NULL);
    } else if (icon == UI_ICON_KEYPAD) {
        lv_obj_add_event_cb(card, unlock_go_cb, LV_EVENT_CLICKED, NULL);
    }

    if (out_card) *out_card = card;
}

/**
 * @brief 构造一张信息卡：左侧矢量图标 / 右侧标题+数值
 * 标题色 = TH_TEXT_MUT（小、灰），数值色 = TH_TEXT（大、亮）
 */
static void build_info_card(lv_obj_t * parent,
                            ui_icon_kind_t icon,
                            const char * title,
                            const char * default_value,
                            lv_obj_t ** out_value,
                            lv_obj_t ** out_icon)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_add_style(card, &st_panel, 0);
    lv_obj_set_style_radius(card, SX(14), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, SX(14), 0);
    lv_obj_set_style_pad_column(card, SX(14), 0);

    *out_icon = ui_icon_create(card, icon, SX(36), theme_color(TH_TEXT));

    /* 右侧文本堆叠 */
    lv_obj_t * right = lv_obj_create(card);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_outline_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_row(right, SY(4), 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(right, 1);

    lv_obj_t * t = lv_label_create(right);
    lv_label_set_text(t, title);
    lv_obj_add_style(t, &st_text_mut, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(13), 0);

    *out_value = lv_label_create(right);
    lv_label_set_text(*out_value, default_value);
    lv_obj_add_style(*out_value, &st_text, 0);
    lv_obj_set_style_text_font(*out_value, app_font_scaled(18), 0);
}

/**
 * @brief 业务函数：统计今天的日志事件数量
 */
static int count_today_events(void)
{
    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query(NULL, -1, &entries, &n) != 0) return 0;

    time_t now = (time_t)hal_time();
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);

    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(entries[i].ts, today, 10) == 0) cnt++;
    }
    free(entries);
    return cnt;
}

/* =========================================================================
 * 异步后台统计逻辑
 * ========================================================================= */

typedef struct {
    int  user_count;
    int  event_count;
    char last_open[48];
    bool net_connected;
} monitor_stats_t;

static bool s_stats_busy = false;

static void stats_worker(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;

    safe_user_t * us = NULL;
    int uc = 0;
    if (user_load_all(&us, &uc) == 0) {
        a->user_count = uc;
        user_list_free(us);
    }

    a->event_count = count_today_events();
    a->net_connected = true;   /* PC 阶段假定常连；板子接 net_is_online() */

    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query("unlock", 1, &entries, &n) == 0 && n > 0) {
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：%.5s %.5s",
                 entries[0].ts + 5, entries[0].ts + 11);
        free(entries);
    } else {
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：--");
    }
}

static void stats_done(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;

    s_stats_busy = false;

    char buf[16];

    snprintf(buf, sizeof(buf), "%d 人", a->user_count);
    lv_label_set_text(s_user_value, buf);

    snprintf(buf, sizeof(buf), "%d 条", a->event_count);
    lv_label_set_text(s_event_value, buf);

    if (a->net_connected) {
        lv_label_set_text(s_net_value, "已连接");
    } else {
        lv_label_set_text(s_net_value, "未连接");
    }

    lv_label_set_text(s_last_label, a->last_open);

    free(a);
}

/* "当前是否开锁" 判定：
 *   - 物理执行器高电平（hal_actuator_state()=true），或
 *   - FSM 处于 UNLOCKED 状态（最近 30s 内通过 PIN/人脸/动态码成功开锁）
 * 两路任意一路为真就显示"已开启"。这是修复 v3.1 反馈"密码开锁后仍显示
 * 已上锁"bug 的关键 —— 之前只查 hal_actuator_state()，但物理执行器
 * 500ms 后会自动拉低（NFR-7 硬件触发），UI 立即跳回"已上锁"。
 * 现在：执行器是物理脉冲 500ms，FSM 是 UI 语义 30s，谁先到谁就位。 */
static bool is_unlocked_now(void)
{
    return hal_actuator_state() || auth_fsm_state() == FSM_UNLOCKED;
}

/* 把"根据锁状态刷新控件颜色"独立出来：既被 timer 调用，也被主题切换回调调用 */
static void monitor_refresh_state_colors(void)
{
    bool open = is_unlocked_now();

    if (s_lock_ring) {
        lv_obj_set_style_border_color(s_lock_ring,
                                      open ? theme_color(TH_OK) : theme_color(TH_DANGER), 0);
    }
    if (s_lock_icon) {
        ui_icon_set_color(s_lock_icon,
                          open ? theme_color(TH_OK) : theme_color(TH_DANGER));
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, open ? "保险柜已开启" : "保险柜已上锁");
    }
}

/* 主题切换回调：让主页圆环/锁头/卡片的本地颜色覆盖也随主题刷新 */
static void monitor_refresh_theme(int idx)
{
    (void)idx;
    monitor_refresh_state_colors();
    /* 卡片主题色（信息/开锁卡） */
    if (s_pin_card) {
        lv_obj_set_style_bg_color(s_pin_card, theme_color(TH_PANEL), 0);
        lv_obj_set_style_border_color(s_pin_card, theme_color(TH_BORDER), 0);
    }
    if (s_face_card) {
        lv_obj_set_style_bg_color(s_face_card, theme_color(TH_PANEL), 0);
        lv_obj_set_style_border_color(s_face_card, theme_color(TH_BORDER), 0);
    }
    if (s_pin_icon) ui_icon_set_color(s_pin_icon, theme_color(TH_ACCENT));
    if (s_face_icon) ui_icon_set_color(s_face_icon, theme_color(TH_ACCENT));
    if (s_user_icon) ui_icon_set_color(s_user_icon, theme_color(TH_TEXT));
    if (s_event_icon) ui_icon_set_color(s_event_icon, theme_color(TH_TEXT));
    if (s_net_icon) ui_icon_set_color(s_net_icon, theme_color(TH_TEXT));
    /* 注：人脸卡角标 KEY_LOCKED 已移除（避免卡片内元素拥挤），如需要可重新加回。 */
}

static void monitor_timer_cb(lv_timer_t * t)
{
    (void)t;
    monitor_refresh_state_colors();

    if (s_stats_busy) return;

    s_stats_busy = true;

    monitor_stats_t * a = (monitor_stats_t *)malloc(sizeof(*a));
    if (!a) {
        s_stats_busy = false;
        return;
    }
    memset(a, 0, sizeof(*a));

    worker_post(stats_worker, a, stats_done);
}

/* 主页"密码开锁"按钮回调：跳转到 PIN 键盘（解锁）页 */
static void unlock_go_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_KEYPAD);
}

/* 主页"人脸识别"按钮回调：跳转到独立的人脸识别全屏页 */
static void face_unlock_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_FACE);
}
