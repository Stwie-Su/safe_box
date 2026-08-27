/**
 * @file page_logs.c
 * 日志页（DESIGN.md §7.7 / §3.3）：时间 + 事件 + 用户 + 结果，支持按事件/结果筛选。
 * 数据源：core/store 的 safe.log（JSON Lines）。
 */
#include "page_logs.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/async_store.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 筛选选项 */
typedef enum {
    FIL_ALL = 0,   /* 全部 */
    FIL_UNLOCK,    /* 开锁 */
    FIL_USER,      /* 用户 */
    FIL_SETTING,   /* 设置 */
    FIL_FAIL,      /* 失败 */
    FIL_COUNT
} log_filter_t;

static const char * const FIL_LABELS[FIL_COUNT] = { "全部", "开锁", "用户", "设置", "失败" };

static lv_obj_t * s_list;
static lv_obj_t * s_summary;
static log_filter_t s_filter = FIL_ALL;
static lv_obj_t * s_filter_btns[FIL_COUNT];

static void build_filter_bar(lv_obj_t * parent);
static void filter_click_cb(lv_event_t * e);
static void logs_timer_cb(lv_timer_t * t);
static void rebuild_list(void);

/* 事件前缀匹配 */
static bool evt_matches(const char *evt, log_filter_t f)
{
    switch (f) {
        case FIL_ALL:     return true;
        case FIL_UNLOCK:  return strncmp(evt, "unlock", 6) == 0 || strcmp(evt, "lock") == 0;
        case FIL_USER:    return strncmp(evt, "user", 4) == 0;
        case FIL_SETTING: return strncmp(evt, "setting", 7) == 0 ||
                                  strncmp(evt, "pwd", 3) == 0 ||
                                  strncmp(evt, "wifi", 4) == 0 ||
                                  strcmp(evt, "factory_reset") == 0;
        case FIL_FAIL:    return true;   /* 由 res 判断 */
        default:          return true;
    }
}

lv_obj_t * page_logs_create(lv_obj_t * parent)
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

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "日志记录");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font(28), 0);

    s_summary = lv_label_create(head);
    lv_obj_add_style(s_summary, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_summary, app_font(14), 0);
    lv_obj_set_style_pad_left(s_summary, 16, 0);

    /* 筛选条 */
    build_filter_bar(root);

    /* 列表 */
    s_list = lv_obj_create(root);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_add_style(s_list, &st_panel, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* 周期刷新（日志为追加型，轮询开销小） */
    lv_timer_create(logs_timer_cb, 1000, root);
    rebuild_list();
    return root;
}

static void build_filter_bar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);

    int i;
    for (i = 0; i < FIL_COUNT; i++) {
        lv_obj_t * btn = lv_button_create(bar);
        lv_obj_set_height(btn, 36);
        lv_obj_set_style_pad_left(btn, 14, 0);
        lv_obj_set_style_pad_right(btn, 14, 0);
        lv_obj_set_style_pad_top(btn, 0, 0);
        lv_obj_set_style_pad_bottom(btn, 0, 0);
        lv_obj_add_style(btn, &st_ghost_btn, 0);
        lv_obj_add_style(btn, &st_accent_btn, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_border_width(btn, 1, LV_STATE_CHECKED);

        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, FIL_LABELS[i]);
        lv_obj_set_style_text_font(lbl, app_font(14), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, filter_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_filter_btns[i] = btn;
        if (i == FIL_ALL) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }
}

static void filter_click_cb(lv_event_t * e)
{
    log_filter_t f = (log_filter_t)(uintptr_t)lv_event_get_user_data(e);
    if (f == s_filter) return;
    s_filter = f;
    for (int i = 0; i < FIL_COUNT; i++) {
        if (i == (int)f) lv_obj_add_state(s_filter_btns[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_filter_btns[i], LV_STATE_CHECKED);
    }
    rebuild_list();
}

/* 异步查询：文件读取+JSON 解析在后台，回调在主线程渲染（DESIGN.md §9） */
static void logs_loaded(log_entry_t * all, int n)
{
    lv_obj_clean(s_list);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const log_entry_t * e = &all[i];
        if (s_filter == FIL_FAIL && e->res != 0) continue;
        if (!evt_matches(e->evt, s_filter)) continue;

        /* 一行：时间 | 事件 | 用户 | 结果 */
        lv_obj_t * row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_add_style(row, &st_panel2, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t * ts = lv_label_create(row);
        lv_label_set_text(ts, e->ts);
        lv_obj_add_style(ts, &st_text_mut, 0);
        lv_obj_set_style_text_font(ts, app_font(14), 0);

        lv_obj_t * ev = lv_label_create(row);
        lv_label_set_text(ev, e->evt);
        lv_obj_add_style(ev, &st_text, 0);
        lv_obj_set_style_text_font(ev, app_font(14), 0);
        lv_obj_set_style_pad_left(ev, 20, 0);

        lv_obj_t * us = lv_label_create(row);
        lv_label_set_text(us, e->user);
        lv_obj_add_style(us, &st_text_mut, 0);
        lv_obj_set_style_text_font(us, app_font(14), 0);
        lv_obj_set_style_pad_left(us, 20, 0);

        lv_obj_t * rs = lv_label_create(row);
        lv_label_set_text(rs, e->res ? "成功" : "失败");
        lv_obj_add_style(rs, e->res ? &st_ok_text : &st_danger_text, 0);
        lv_obj_set_style_text_font(rs, app_font(14), 0);
        lv_obj_set_style_pad_left(rs, 20, 0);

        lv_obj_t * dt = lv_label_create(row);
        lv_label_set_text(dt, e->detail);
        lv_obj_add_style(dt, &st_text_mut, 0);
        lv_obj_set_style_text_font(dt, app_font(14), 0);
        lv_obj_set_style_pad_left(dt, 20, 0);
        lv_obj_set_flex_grow(dt, 1);

        shown++;
        if (shown >= 200) break;   /* 防超长列表卡顿 */
    }
    if (shown == 0) {
        lv_obj_t * lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "没有符合条件的记录");
        lv_obj_add_style(lbl, &st_text_mut, 0);
        lv_obj_set_style_text_font(lbl, app_font(14), 0);
    }
    char sum[48];
    snprintf(sum, sizeof(sum), "共 %d 条", shown);
    lv_label_set_text(s_summary, sum);
    /* 列表由 astore 框架释放 */
}

static void rebuild_list(void)
{
    astore_query_log(NULL, -1, logs_loaded);
}

static void logs_timer_cb(lv_timer_t * t)
{
    (void)t;
    rebuild_list();
}
