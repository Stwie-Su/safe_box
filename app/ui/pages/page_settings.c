/**
 * @file page_settings.c
 * 开发者选项（原「设置中枢」，2026-09-14 需求变更 FR-7）。
 *
 * 变更理由：主导航（rail）本就有 用户/网络/系统 七个一级入口，
 * 「设置中枢」的三个入口卡片与 rail 完全重复，且其上的管理员二次验证
 * 形同虚设（从 rail 点「用户」即可绕过）→ 页面冗余、鉴权口径不一致。
 *
 * 现语义：**只读诊断页**——集中展示构建/运行/通道/存储的真实状态，
 * 不含任何业务功能；业务操作仍在 主页/用户/日志/人脸/网络/系统 各页，
 * 并由各页内的敏感操作二次验证把关（用户增删改 / 策略修改 / 恢复出厂）。
 *
 * 数据来源（全部真实接口，无占位符）：
 *   app_config()            —— face_backend / mqtt_host:port / log_max_entries
 *   mqtt_is_connected()     —— MQTT 在线状态
 *   hal_time()/hal_time_ms()/hal_time_source() —— 时钟、运行时长、时钟源
 *   log_query()             —— 当前日志条数（**仅在创建与手动刷新时读一次**，
 *                              不放周期定时器：log_query 是全文件读取，
 *                              每秒轮询会把 O(cap) 读放大搬进 UI 线程）
 *   store_dir()             —— 数据目录
 *
 * 颜色纪律：零 hex；层次 = 1px TH_BORDER + 表面色阶，不用模糊阴影。
 */
#include "page_settings.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/config.h"
#include "core/remote/mqtt_client.h"
#include "hal/hal_time.h"
#include "app_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t * s_clock_lbl;      /* 标题行实时时钟 */
static lv_obj_t * s_uptime_lbl;     /* 系统运行时长（每秒刷新） */
static lv_obj_t * s_mqtt_lbl;       /* MQTT 状态（每秒刷新） */
static lv_obj_t * s_logcnt_lbl;     /* 当前日志条数（创建/手动刷新时读） */
static lv_obj_t * s_logcap_lbl;     /* 日志保留上限（随配置） */

static void clock_timer_cb(lv_timer_t * t);
static void refresh_log_count(void);
static void refresh_btn_cb(lv_event_t * e);

/** 只读信息行：左灰标签、右值；返回值 label 便于后续 set_text。 */
static lv_obj_t * info_row(lv_obj_t * parent, const char * name, lv_obj_t ** val_out)
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

/** 分区小标题 */
static lv_obj_t * card_title(lv_obj_t * parent, const char * txt)
{
    lv_obj_t * t = lv_label_create(parent);
    lv_label_set_text(t, txt);
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(18), 0);
    return t;
}

lv_obj_t * page_settings_create(lv_obj_t * parent)
{
    const app_config_t * cfg = app_config();

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, SX(18), 0);
    lv_obj_set_style_pad_row(root, SY(10), 0);

    /* ---------- 标题行 ---------- */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t * tt = lv_obj_create(head);
    lv_obj_set_size(tt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tt, 0, 0);
    lv_obj_set_style_pad_all(tt, 0, 0);
    lv_obj_set_scrollable(tt, false);
    lv_obj_set_flex_flow(tt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tt, 2, 0);

    lv_obj_t * title = lv_label_create(tt);
    lv_label_set_text(title, "开发者选项");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(26), 0);

    lv_obj_t * subtitle = lv_label_create(tt);
    lv_label_set_text(subtitle, "只读诊断信息 · 不含业务功能");
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

    /* ---------- 双栏 ---------- */
    lv_obj_t * body = lv_obj_create(root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, SX(14), 0);

    /* --- 左：构建与运行 --- */
    lv_obj_t * left = lv_obj_create(body);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_add_style(left, &st_panel, 0);
    lv_obj_set_scrollable(left, false);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);

    card_title(left, "构建与运行");

    lv_obj_t * v_fw = NULL;
    info_row(left, "固件版本", &v_fw);
    if (v_fw) lv_label_set_text(v_fw, SAFE_VERSION_STRING);

    lv_obj_t * v_be = NULL;
    info_row(left, "人脸后端", &v_be);
    if (v_be) lv_label_set_text(v_be, cfg->face_backend);

    lv_obj_t * v_src = NULL;
    info_row(left, "时钟源", &v_src);
    if (v_src) lv_label_set_text(v_src, hal_time_source() == TIME_SRC_RTC ? "rtc（DS3231）" : "sys（系统时间）");

    lv_obj_t * v_theme = NULL;
    info_row(left, "当前主题", &v_theme);
    if (v_theme) lv_label_set_text(v_theme, theme_name(theme_idx()));

    lv_obj_t * v_up = NULL;
    info_row(left, "系统运行时长", &v_up);
    s_uptime_lbl = v_up;

    /* --- 右：通道与存储 --- */
    lv_obj_t * right = lv_obj_create(body);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_add_style(right, &st_panel, 0);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);

    card_title(right, "通道与存储");

    lv_obj_t * v_mqtt = NULL;
    info_row(right, "MQTT 通道", &v_mqtt);
    s_mqtt_lbl = v_mqtt;

    lv_obj_t * v_cap = NULL;
    info_row(right, "日志保留上限", &v_cap);
    s_logcap_lbl = v_cap;

    lv_obj_t * v_cnt = NULL;
    info_row(right, "当前日志条数", &v_cnt);
    s_logcnt_lbl = v_cnt;

    lv_obj_t * v_dir = NULL;
    info_row(right, "数据目录", &v_dir);
    if (v_dir) lv_label_set_text(v_dir, store_dir());

    /* 弹性占位：把刷新按钮压到卡底 */
    lv_obj_t * rspacer = lv_obj_create(right);
    lv_obj_set_width(rspacer, lv_pct(100));
    lv_obj_set_flex_grow(rspacer, 1);
    lv_obj_set_style_bg_opa(rspacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rspacer, 0, 0);
    lv_obj_set_scrollable(rspacer, false);

    lv_obj_t * refresh = ui_icon_text_button(right, LV_SYMBOL_REFRESH, "刷新诊断数据",
                                             lv_pct(100), SY(40), &st_ghost_btn,
                                             theme_color(TH_TEXT), refresh_btn_cb, NULL);
    (void)refresh;

    refresh_log_count();
    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
    return root;
}

/* 时钟 / 运行时长 / MQTT 状态：轻量值，每秒刷新安全 */
static void clock_timer_cb(lv_timer_t * t)
{
    (void)t;
    char buf[96];

    time_t now = (time_t)hal_time();
    struct tm * tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmv);
        lv_label_set_text(s_clock_lbl, buf);
    }

    /* 运行时长：hal_time_ms() 为单调毫秒（uint32，约 49.7 天回绕，嵌入式可接受） */
    uint32_t up_s = hal_time_ms() / 1000u;
    snprintf(buf, sizeof(buf), "开机 %u 时 %02u 分 %02u 秒",
             (unsigned)(up_s / 3600u), (unsigned)((up_s / 60u) % 60u), (unsigned)(up_s % 60u));
    lv_label_set_text(s_uptime_lbl, buf);

    snprintf(buf, sizeof(buf), "%s:%d · %s",
             app_config()->mqtt_host, app_config()->mqtt_port,
             mqtt_is_connected() ? "已连接" : "断线");
    lv_label_set_text(s_mqtt_lbl, buf);
    lv_obj_add_style(s_mqtt_lbl, mqtt_is_connected() ? &st_ok_text : &st_danger_text, 0);
}

/* 当前日志条数：log_query 为全文件读取，只在创建与手动刷新时调用 */
static void refresh_log_count(void)
{
    log_entry_t * rows = NULL;
    int n = 0;
    int rc = log_query(NULL, -1, &rows, &n);
    if (rc == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d 条", n);
        lv_label_set_text(s_logcnt_lbl, buf);
        free(rows);
    } else {
        lv_label_set_text(s_logcnt_lbl, "读取失败");
    }
    if (s_logcap_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d 条（SAFE_LOG_MAX 可调）", app_config()->log_max_entries);
        lv_label_set_text(s_logcap_lbl, buf);
    }
}

static void refresh_btn_cb(lv_event_t * e)
{
    (void)e;
    refresh_log_count();
}
