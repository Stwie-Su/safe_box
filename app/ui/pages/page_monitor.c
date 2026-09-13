/**
 * @file page_monitor.c
 * 主页（ui5 仪表盘重设计）：锁状态 hero 卡 + 4 统计卡 + 最近事件列表。
 *
 * 视觉真源：ui_redesign_preview.html 的 #screen-home（hero 卡 / .grid 4 卡 /
 * .panel 事件列表）。实现时把「设计稿像素」全部走 SX()/SY() 缩放，颜色一律
 * theme_color(token)，四套主题自动跟随，零硬编码 hex。
 *
 * 布局（内容区 = 屏宽 − rail 104，屏高 − 顶栏 56）：
 *   1) hero 卡（h 120）：左侧 64px 圆角状态徽标（锁图标，色随锁状态）+
 *      中间两行（状态主标题 / 最后开启 + 开锁方式）+ 右侧两只按钮
 *      （一键开锁=accent 实心 / 密码开锁=ghost 描边，PIN 通道必须可达）
 *   2) 4 统计卡（h 108）：注册用户 / 今日事件 / 人脸识别 / 存储占用
 *   3) 最近事件面板（占满剩余高度）：图标按 ok/warn/danger 着色 +
 *      主文案 + 灰色次文案 + 时间
 *
 * 数据真实性（全部走既有 store / HAL 接口，无占位符）：
 *   - 注册用户：user_load_all() 计数
 *   - 今日事件：log_query() 全量后按当天日期过滤计数
 *   - 人脸识别：user_load_all() 中 face_enable && face_id>=0 的人数
 *   - 存储占用：statvfs() 统计数据目录所在文件系统的已用百分比
 *   - 最近事件：log_query() 返回「倒序（最新在前）」，取前 4 条
 *   - 最后开启：log_query("UNLOCK", 1) 的第一条
 *     （旧版写的是小写 "unlock"，与 auth_fsm.c 实际写入的 "UNLOCK" 不匹配，
 *      导致「最后开启」永远显示 "--"，本版顺带修正）
 *   - 开锁方式：由用户数据推导（PIN 恒有 / 人脸 / 动态码按开关）
 *
 * 性能与纪律：
 *   - 统计走既有 worker 线程（worker_post），UI 线程只做 set_text，不阻塞渲染；
 *   - 统计结果放在静态结构里（s_stats_busy 互斥），**无每帧 / 每秒堆分配**；
 *   - 事件行对象一次性创建 4 行，刷新只改 text / 图标码位 / 颜色。
 */
#include "page_monitor.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"       /* SX/SY 自适应缩放 */
#include "ui/icons.h"          /* 图标字体 label（icon_label_colored / set_glyph） */
#include "core/store/store.h"
#include "core/support/worker.h"
#include "core/auth/auth_fsm.h"
#include "hal/hal_actuator.h"
#include "hal/hal_time.h"     /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>      /* 存储占用：统计真实文件系统使用率 */

#define EV_ROWS   4           /* 最近事件最多展示行数（= 预建行数） */

/* ================== 事件行视图（worker 侧算好，UI 线程只贴值） ================== */
typedef struct {
    ui_glyph_t   glyph;
    theme_role_t role;
    char         title[40];
    char         sub[72];
    char         time[16];
} ev_view_t;

/* ================== 全局静态 UI 对象指针 ================== */
/* hero */
static lv_obj_t * s_hero_card;
static lv_obj_t * s_hero_iconbox;   /* 64px 状态徽标盒 */
static lv_obj_t * s_hero_icon;      /* 锁图标（图标字体 label） */
static lv_obj_t * s_status_label;
static lv_obj_t * s_sub_label;
static lv_obj_t * s_btn_fast;       /* 一键开锁 */
static lv_obj_t * s_btn_fast_icon;
static lv_obj_t * s_btn_fast_label;
static lv_obj_t * s_btn_pin;        /* 密码开锁 */
static lv_obj_t * s_btn_pin_icon;
static lv_obj_t * s_btn_pin_label;
/* 4 统计卡 */
static lv_obj_t * s_stat_card[4];
static lv_obj_t * s_stat_box[4];
static lv_obj_t * s_stat_icon[4];
static lv_obj_t * s_stat_value[4];
/* 最近事件面板 */
static lv_obj_t * s_panel;
typedef struct {
    lv_obj_t * box;
    lv_obj_t * icon;
    lv_obj_t * title;
    lv_obj_t * sub;
    lv_obj_t * time;
} ev_row_t;
static ev_row_t s_rows[EV_ROWS];

/* ================== 异步统计 ================== */
typedef struct {
    int       user_count;
    int       event_count;
    int       face_count;     /* 已录入人脸的用户数 */
    int       storage_pct;
    bool      totp_ready;     /* 是否有人启用动态码 */
    char      last_open[32];
    char      methods[48];    /* "PIN / 人脸 / 动态码" */
    int       ev_n;
    ev_view_t ev[EV_ROWS];
} monitor_stats_t;

static monitor_stats_t s_stats;     /* 静态，避免每秒 malloc（见文件头性能说明） */
static bool            s_stats_busy = false;

/* ================== 内部私有函数声明 ================== */
static void pm_no_scroll(lv_obj_t * o);
static void build_hero(lv_obj_t * parent);
static void build_stats(lv_obj_t * parent);
static void build_events(lv_obj_t * parent);
static lv_obj_t * make_panel(lv_obj_t * parent, int32_t h);
static lv_obj_t * make_button(lv_obj_t * parent, ui_glyph_t g, const char * text,
                              bool primary, lv_event_cb_t cb,
                              lv_obj_t ** out_icon, lv_obj_t ** out_label);
static void monitor_timer_cb(lv_timer_t * t);
static void stats_worker(void * p);
static void stats_done(void * p);
static void monitor_refresh_theme(int idx);
static void apply_local_colors(void);
static bool is_unlocked_now(void);
static int  count_today_events(void);
static void fmt_when(const char * ts, char * out, size_t cap);
static void fmt_ev_time(const char * ts, char * out, size_t cap);
static void describe_event(const log_entry_t * e, ev_view_t * v);
static void fast_unlock_cb(lv_event_t * e);
static void pin_unlock_cb(lv_event_t * e);

/**
 * @brief 创建监控主页面（由 UI 框架调用）
 */
lv_obj_t * page_monitor_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_pad_left(root, SX(22), 0);
    lv_obj_set_style_pad_right(root, SX(22), 0);
    lv_obj_set_style_pad_top(root, SY(14), 0);
    lv_obj_set_style_pad_bottom(root, SY(14), 0);
    lv_obj_set_style_pad_row(root, SY(14), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    pm_no_scroll(root);

    build_hero(root);
    build_stats(root);
    build_events(root);

    apply_local_colors();

    lv_timer_create(monitor_timer_cb, 1000, NULL);
    monitor_timer_cb(NULL);

    theme_register_change_cb(monitor_refresh_theme);
    return root;
}

/* ---------------------------------------------------------------------------
 *  通用构件
 * ------------------------------------------------------------------------- */

/* 容器统一禁用滚动：仪表盘是固定布局，出现滚动条既影响观感也会多一层裁剪
 * 绘制（A7 单核），所有容器一次性关掉。 */
static void pm_no_scroll(lv_obj_t * o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* 卡片底板：st_panel（主题托管背景）+ 1px TH_BORDER 描边 + 圆角 18 */
static lv_obj_t * make_panel(lv_obj_t * parent, int32_t h)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), h);
    lv_obj_add_style(card, &st_panel, 0);
    lv_obj_set_style_radius(card, SX(18), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    pm_no_scroll(card);
    return card;
}

/* 小圆角图标盒（统计卡 36 / 事件行 34），底色 TH_PANEL2（surface-3 效果） */
static lv_obj_t * make_icon_box(lv_obj_t * parent, int32_t size, int32_t radius)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, SX(size), SX(size));
    lv_obj_set_style_bg_color(box, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, SX(radius), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    pm_no_scroll(box);
    return box;
}

/* 按钮：primary=accent 实心（一键开锁）/ 否则 ghost 描边（密码开锁） */
static lv_obj_t * make_button(lv_obj_t * parent, ui_glyph_t g, const char * text,
                              bool primary, lv_event_cb_t cb,
                              lv_obj_t ** out_icon, lv_obj_t ** out_label)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_size(btn, SX(152), SY(primary ? 46 : 36));
    lv_obj_set_style_radius(btn, SX(14), 0);
    if (primary) {
        lv_obj_set_style_bg_color(btn, theme_color(TH_ACCENT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, theme_color(TH_BORDER), 0);
    }
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, SX(8), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    *out_icon = icon_label_colored(btn, g, 20, primary ? TH_ACCENT_INK : TH_TEXT);

    *out_label = lv_label_create(btn);
    lv_label_set_text(*out_label, text);
    lv_obj_set_style_text_font(*out_label, app_font_scaled(15), 0);
    lv_obj_set_style_text_color(*out_label, theme_color(primary ? TH_ACCENT_INK : TH_TEXT), 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ---------------------------------------------------------------------------
 *  1) hero 卡
 * ------------------------------------------------------------------------- */
static void build_hero(lv_obj_t * parent)
{
    s_hero_card = make_panel(parent, SY(120));
    lv_obj_set_style_pad_left(s_hero_card, SX(18), 0);
    lv_obj_set_style_pad_right(s_hero_card, SX(18), 0);
    lv_obj_set_style_pad_top(s_hero_card, SY(12), 0);
    lv_obj_set_style_pad_bottom(s_hero_card, SY(12), 0);
    lv_obj_set_style_pad_column(s_hero_card, SX(16), 0);
    lv_obj_set_flex_flow(s_hero_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hero_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    /* 左：64px 状态徽标盒（底色 = 状态色 @20%，图标 = 状态色） */
    s_hero_iconbox = make_icon_box(s_hero_card, 64, 18);
    lv_obj_set_style_bg_opa(s_hero_iconbox, LV_OPA_20, 0);
    s_hero_icon = icon_label_colored(s_hero_iconbox, UI_GLYPH_LOCK, 34, TH_DANGER);
    lv_obj_center(s_hero_icon);

    /* 中：状态主标题 + 副信息（最后开启 / 开锁方式） */
    lv_obj_t * meta = lv_obj_create(s_hero_card);
    lv_obj_set_flex_grow(meta, 1);
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta, 0, 0);
    lv_obj_set_style_outline_width(meta, 0, 0);
    lv_obj_set_style_pad_all(meta, 0, 0);
    lv_obj_set_style_pad_row(meta, SY(6), 0);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pm_no_scroll(meta);

    s_status_label = lv_label_create(meta);
    lv_label_set_text(s_status_label, "保险柜已上锁");
    lv_obj_add_style(s_status_label, &st_text, 0);
    lv_obj_set_style_text_font(s_status_label, app_font_scaled(20), 0);

    s_sub_label = lv_label_create(meta);
    lv_label_set_text(s_sub_label, "最后开启 · --　·　开锁方式 PIN");
    lv_obj_add_style(s_sub_label, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_sub_label, app_font_scaled(13), 0);

    /* 右：一键开锁（accent 实心）+ 密码开锁（ghost 描边） */
    lv_obj_t * acts = lv_obj_create(s_hero_card);
    lv_obj_set_size(acts, SX(152), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(acts, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(acts, 0, 0);
    lv_obj_set_style_outline_width(acts, 0, 0);
    lv_obj_set_style_pad_all(acts, 0, 0);
    lv_obj_set_style_pad_row(acts, SY(8), 0);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pm_no_scroll(acts);

    s_btn_fast = make_button(acts, UI_GLYPH_FINGER, "一键开锁", true, fast_unlock_cb,
                             &s_btn_fast_icon, &s_btn_fast_label);
    s_btn_pin = make_button(acts, UI_GLYPH_PIN, "密码开锁", false, pin_unlock_cb,
                            &s_btn_pin_icon, &s_btn_pin_label);
}

/* ---------------------------------------------------------------------------
 *  2) 4 张统计卡
 * ------------------------------------------------------------------------- */
typedef struct {
    ui_glyph_t   glyph;
    const char * key;
} stat_def_t;

static const stat_def_t STATS[4] = {
    { UI_GLYPH_PERSON,  "注册用户" },
    { UI_GLYPH_BELL,    "今日事件" },
    { UI_GLYPH_FACE,    "人脸识别" },
    { UI_GLYPH_STORAGE, "存储占用" },
};

static void build_stats(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), SY(108));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SX(14), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    pm_no_scroll(row);

    for (int i = 0; i < 4; i++) {
        lv_obj_t * card = make_panel(row, lv_pct(100));
        lv_obj_set_width(card, SX(196));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_pad_left(card, SX(14), 0);
        lv_obj_set_style_pad_right(card, SX(14), 0);
        lv_obj_set_style_pad_top(card, SY(12), 0);
        lv_obj_set_style_pad_bottom(card, SY(12), 0);
        lv_obj_set_style_pad_row(card, SY(6), 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        s_stat_card[i] = card;

        s_stat_box[i] = make_icon_box(card, 36, 11);
        s_stat_icon[i] = icon_label_colored(s_stat_box[i], STATS[i].glyph, 20, TH_ACCENT);
        lv_obj_center(s_stat_icon[i]);

        s_stat_value[i] = lv_label_create(card);
        lv_label_set_text(s_stat_value[i], "--");
        lv_obj_add_style(s_stat_value[i], &st_text, 0);
        lv_obj_set_style_text_font(s_stat_value[i], app_font_scaled(20), 0);

        lv_obj_t * k = lv_label_create(card);
        lv_label_set_text(k, STATS[i].key);
        lv_obj_add_style(k, &st_text_mut, 0);
        lv_obj_set_style_text_font(k, app_font_scaled(13), 0);
    }
}

/* ---------------------------------------------------------------------------
 *  3) 最近事件面板
 * ------------------------------------------------------------------------- */
static void build_events(lv_obj_t * parent)
{
    s_panel = make_panel(parent, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_panel, 1);
    lv_obj_set_style_pad_left(s_panel, SX(14), 0);
    lv_obj_set_style_pad_right(s_panel, SX(14), 0);
    lv_obj_set_style_pad_top(s_panel, SY(8), 0);
    lv_obj_set_style_pad_bottom(s_panel, SY(8), 0);
    lv_obj_set_style_pad_row(s_panel, SY(4), 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * ttl = lv_label_create(s_panel);
    lv_label_set_text(ttl, "最近事件");
    lv_obj_add_style(ttl, &st_text_mut, 0);
    lv_obj_set_style_text_font(ttl, app_font_scaled(13), 0);

    for (int i = 0; i < EV_ROWS; i++) {
        lv_obj_t * r = lv_obj_create(s_panel);
        lv_obj_set_size(r, lv_pct(100), SY(46));
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_side(r, i == EV_ROWS - 1 ? LV_BORDER_SIDE_NONE : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(r, theme_color(TH_BORDER), 0);
        lv_obj_set_style_outline_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_set_style_pad_column(r, SX(12), 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        pm_no_scroll(r);

        s_rows[i].box = make_icon_box(r, 34, 10);
        s_rows[i].icon = icon_label_colored(s_rows[i].box, UI_GLYPH_BELL, 18, TH_TEXT_MUT);
        lv_obj_center(s_rows[i].icon);

        lv_obj_t * mid = lv_obj_create(r);
        lv_obj_set_flex_grow(mid, 1);
        lv_obj_set_height(mid, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mid, 0, 0);
        lv_obj_set_style_outline_width(mid, 0, 0);
        lv_obj_set_style_pad_all(mid, 0, 0);
        lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        pm_no_scroll(mid);

        s_rows[i].title = lv_label_create(mid);
        lv_label_set_text(s_rows[i].title, "--");
        lv_obj_add_style(s_rows[i].title, &st_text, 0);
        lv_obj_set_style_text_font(s_rows[i].title, app_font_scaled(14), 0);

        s_rows[i].sub = lv_label_create(mid);
        lv_label_set_text(s_rows[i].sub, "");
        lv_obj_add_style(s_rows[i].sub, &st_text_mut, 0);
        lv_obj_set_style_text_font(s_rows[i].sub, app_font_scaled(13), 0);

        s_rows[i].time = lv_label_create(r);
        lv_label_set_text(s_rows[i].time, "--:--");
        lv_obj_set_width(s_rows[i].time, SX(56));
        lv_obj_add_style(s_rows[i].time, &st_text_mut, 0);
        lv_obj_set_style_text_font(s_rows[i].time, app_font_scaled(13), 0);
        lv_obj_set_style_text_align(s_rows[i].time, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ---------------------------------------------------------------------------
 *  本地颜色覆盖：切主题 / 锁状态变化时统一重刷
 *  （背景与正文色走 st_panel / st_text / st_text_mut，由 theme_switch 自动刷新，
 *    这里只处理「按角色取色」的那些本地覆盖）
 * ------------------------------------------------------------------------- */
static void apply_local_colors(void)
{
    bool open = is_unlocked_now();
    lv_color_t st = theme_color(open ? TH_OK : TH_DANGER);

    if (s_hero_card)    lv_obj_set_style_border_color(s_hero_card, theme_color(TH_BORDER), 0);
    if (s_hero_iconbox) lv_obj_set_style_bg_color(s_hero_iconbox, st, 0);
    if (s_hero_icon) {
        lv_obj_set_style_text_color(s_hero_icon, st, 0);
        icon_label_set_glyph(s_hero_icon, open ? UI_GLYPH_LOCK_OPEN : UI_GLYPH_LOCK);
    }
    if (s_status_label) lv_label_set_text(s_status_label, open ? "保险柜已开启" : "保险柜已上锁");

    if (s_btn_fast) {
        lv_obj_set_style_bg_color(s_btn_fast, theme_color(TH_ACCENT), 0);
    }
    if (s_btn_fast_icon)  lv_obj_set_style_text_color(s_btn_fast_icon, theme_color(TH_ACCENT_INK), 0);
    if (s_btn_fast_label) lv_obj_set_style_text_color(s_btn_fast_label, theme_color(TH_ACCENT_INK), 0);
    if (s_btn_pin)        lv_obj_set_style_border_color(s_btn_pin, theme_color(TH_BORDER), 0);

    for (int i = 0; i < 4; i++) {
        if (s_stat_card[i])  lv_obj_set_style_border_color(s_stat_card[i], theme_color(TH_BORDER), 0);
        if (s_stat_box[i])   lv_obj_set_style_bg_color(s_stat_box[i], theme_color(TH_PANEL2), 0);
        if (s_stat_icon[i])  lv_obj_set_style_text_color(s_stat_icon[i], theme_color(TH_ACCENT), 0);
    }

    if (s_panel) lv_obj_set_style_border_color(s_panel, theme_color(TH_BORDER), 0);
    for (int i = 0; i < EV_ROWS; i++) {
        if (s_rows[i].box)   lv_obj_set_style_bg_color(s_rows[i].box, theme_color(TH_PANEL2), 0);
        if (s_rows[i].icon)  lv_obj_set_style_text_color(s_rows[i].icon, theme_color(TH_TEXT_MUT), 0);
    }
}

/* ---------------------------------------------------------------------------
 *  真实数据：统计（worker 线程）
 * ------------------------------------------------------------------------- */

/* "当前是否开锁" 判定：
 *   - 物理执行器高电平（hal_actuator_state()=true），或
 *   - FSM 处于 UNLOCKED 状态（最近 30s 内通过 PIN/人脸/动态码成功开锁）
 * 两路任意一路为真就显示"已开启"。执行器是 500ms 物理脉冲，FSM 是 30s UI
 * 语义窗口，谁先到谁就位。 */
static bool is_unlocked_now(void)
{
    return hal_actuator_state() || auth_fsm_state() == FSM_UNLOCKED;
}

/* 今天的日志条数（log_query 返回倒序，过滤当天日期即可） */
static int count_today_events(void)
{
    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query(NULL, -1, &entries, &n) != 0) return 0;

    time_t now = (time_t)hal_time();
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);

    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(entries[i].ts, today, 10) == 0) cnt++;
    }
    free(entries);
    return cnt;
}

/* 存储占用：统计数据目录所在文件系统的已用百分比（真实值，非占位） */
static int storage_used_pct(void)
{
    struct statvfs vfs;
    if (statvfs(store_dir(), &vfs) != 0) return 0;
    if (vfs.f_blocks == 0) return 0;
    uint64_t used = (uint64_t)(vfs.f_blocks - vfs.f_bfree);
    return (int)((used * 100u) / (uint64_t)vfs.f_blocks);
}

/* 时间格式化：今天 → "今天 HH:MM"；昨天 → "昨天 HH:MM"；更早 → "MM-DD HH:MM" */
static void fmt_when(const char * ts, char * out, size_t cap)
{
    if (ts == NULL || ts[0] == '\0') { snprintf(out, cap, "--"); return; }

    time_t now = (time_t)hal_time();
    struct tm tm_now, tm_yday;
    localtime_r(&now, &tm_now);
    time_t y = now - 24 * 3600;
    localtime_r(&y, &tm_yday);
    char today[16], yday[16];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);
    strftime(yday, sizeof(yday), "%Y-%m-%d", &tm_yday);

    char hhmm[8] = "--:--";
    if (strlen(ts) >= 16) { memcpy(hhmm, ts + 11, 5); hhmm[5] = '\0'; }

    if (strncmp(ts, today, 10) == 0)          snprintf(out, cap, "今天 %s", hhmm);
    else if (strncmp(ts, yday, 10) == 0)      snprintf(out, cap, "昨天 %s", hhmm);
    else                                       snprintf(out, cap, "%.5s %s", ts + 5, hhmm);
}

/* 事件列的时间格更短：今天 → "HH:MM"；昨天 → "昨天"；更早 → "MM-DD" */
static void fmt_ev_time(const char * ts, char * out, size_t cap)
{
    if (ts == NULL || ts[0] == '\0') { snprintf(out, cap, "--"); return; }

    time_t now = (time_t)hal_time();
    struct tm tm_now, tm_yday;
    localtime_r(&now, &tm_now);
    time_t y = now - 24 * 3600;
    localtime_r(&y, &tm_yday);
    char today[16], yday[16];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);
    strftime(yday, sizeof(yday), "%Y-%m-%d", &tm_yday);

    if (strncmp(ts, today, 10) == 0) {
        char hhmm[8] = "--:--";
        if (strlen(ts) >= 16) { memcpy(hhmm, ts + 11, 5); hhmm[5] = '\0'; }
        snprintf(out, cap, "%s", hhmm);
    } else if (strncmp(ts, yday, 10) == 0) {
        snprintf(out, cap, "昨天");
    } else {
        snprintf(out, cap, "%.5s", ts + 5);
    }
}

/* 事件 → 图标 + 状态色 + 中文主文案 + 次文案（次文案 = 用户 · 详情） */
static void describe_event(const log_entry_t * e, ev_view_t * v)
{
    v->role  = TH_TEXT_MUT;
    v->glyph = UI_GLYPH_BELL;
    v->title[0] = '\0';
    v->sub[0] = '\0';

    const char * evt = e->evt;
    if (strcmp(evt, "UNLOCK") == 0) {
        v->role  = TH_OK;
        v->glyph = UI_GLYPH_FINGER;
        if (strstr(e->detail, "face"))      v->glyph = UI_GLYPH_FACE;
        else if (strstr(e->detail, "pin"))  v->glyph = UI_GLYPH_PIN;
        else if (strstr(e->detail, "otp"))  v->glyph = UI_GLYPH_QR;
        snprintf(v->title, sizeof(v->title), "开锁成功");
    } else if (strcmp(evt, "DENY") == 0) {
        v->role  = TH_DANGER;
        v->glyph = UI_GLYPH_LOCK;
        snprintf(v->title, sizeof(v->title), "开锁被拒");
    } else if (strcmp(evt, "LOCKOUT") == 0) {
        v->role  = TH_DANGER;
        v->glyph = UI_GLYPH_SHIELD;
        snprintf(v->title, sizeof(v->title), "连续失败已锁定");
    } else if (strcmp(evt, "ALARM") == 0) {
        v->role  = TH_WARN;
        v->glyph = UI_GLYPH_WARN;
        snprintf(v->title, sizeof(v->title), "安防告警");
    } else if (strcmp(evt, "user_add") == 0) {
        v->role  = TH_ACCENT;
        v->glyph = UI_GLYPH_PERSON;
        snprintf(v->title, sizeof(v->title), "新增用户");
    } else if (strcmp(evt, "user_del") == 0) {
        v->role  = TH_ACCENT;
        v->glyph = UI_GLYPH_DELETE;
        snprintf(v->title, sizeof(v->title), "删除用户");
    } else if (strcmp(evt, "user_modify") == 0) {
        v->role  = TH_ACCENT;
        v->glyph = UI_GLYPH_EDIT;
        snprintf(v->title, sizeof(v->title), "修改用户");
    } else if (strcmp(evt, "pwd_change") == 0) {
        v->role  = TH_ACCENT;
        v->glyph = UI_GLYPH_KEY;
        snprintf(v->title, sizeof(v->title), "修改密码");
    } else if (strcmp(evt, "setting_change") == 0) {
        v->role  = TH_TEXT;
        v->glyph = UI_GLYPH_SETTINGS;
        snprintf(v->title, sizeof(v->title), "设置变更");
    } else if (strcmp(evt, "factory_reset") == 0) {
        v->role  = TH_WARN;
        v->glyph = UI_GLYPH_UNDO;
        snprintf(v->title, sizeof(v->title), "恢复出厂设置");
    } else {
        snprintf(v->title, sizeof(v->title), "%s", evt);
    }

    if (e->user[0] && strcmp(e->user, "-") != 0 && e->detail[0])
        snprintf(v->sub, sizeof(v->sub), "%s · %s", e->user, e->detail);
    else if (e->detail[0])
        snprintf(v->sub, sizeof(v->sub), "%s", e->detail);
    else if (e->user[0])
        snprintf(v->sub, sizeof(v->sub), "%s", e->user);

    fmt_ev_time(e->ts, v->time, sizeof(v->time));
}

/* worker 线程：所有磁盘/JSON 读取都在这里，UI 线程只贴值 */
static void stats_worker(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    memset(a, 0, sizeof(*a));

    /* 1) 用户 / 人脸 / 动态码 */
    safe_user_t * us = NULL;
    int uc = 0;
    if (user_load_all(&us, &uc) == 0) {
        a->user_count = uc;
        for (int i = 0; i < uc; i++) {
            if (us[i].face_enable && us[i].face_id >= 0) a->face_count++;
            if (us[i].totp_enable && us[i].totp_secret[0] != '\0') a->totp_ready = true;
        }
        user_list_free(us);
    }

    /* 2) 今日事件 + 存储占用 */
    a->event_count = count_today_events();
    a->storage_pct = storage_used_pct();

    /* 3) 最后开启（注意：auth_fsm.c 写入的事件名是大写 "UNLOCK"） */
    log_entry_t * un = NULL;
    int un_n = 0;
    if (log_query("UNLOCK", 1, &un, &un_n) == 0 && un_n > 0) {
        fmt_when(un[0].ts, a->last_open, sizeof(a->last_open));
        free(un);
    } else {
        if (un) free(un);
        snprintf(a->last_open, sizeof(a->last_open), "暂无记录");
    }

    /* 4) 开锁方式（由真实用户数据推导） */
    snprintf(a->methods, sizeof(a->methods), "PIN%s%s",
             a->face_count > 0 ? " / 人脸" : "",
             a->totp_ready ? " / 动态码" : "");

    /* 5) 最近事件（log_query 返回倒序，取前 EV_ROWS 条） */
    log_entry_t * ev = NULL;
    int ev_n = 0;
    if (log_query(NULL, -1, &ev, &ev_n) == 0) {
        a->ev_n = ev_n < EV_ROWS ? ev_n : EV_ROWS;
        for (int i = 0; i < a->ev_n; i++) describe_event(&ev[i], &a->ev[i]);
        free(ev);
    } else {
        a->ev_n = 0;
    }
}

/* 主线程：把统计结果贴到 UI（只 set_text / set 颜色，不做 IO） */
static void stats_done(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    s_stats_busy = false;

    char buf[32];

    snprintf(buf, sizeof(buf), "%d", a->user_count);
    lv_label_set_text(s_stat_value[0], buf);

    snprintf(buf, sizeof(buf), "%d", a->event_count);
    lv_label_set_text(s_stat_value[1], buf);

    snprintf(buf, sizeof(buf), "%s", a->face_count > 0 ? "已启用" : "未启用");
    lv_label_set_text(s_stat_value[2], buf);

    snprintf(buf, sizeof(buf), "%d%%", a->storage_pct);
    lv_label_set_text(s_stat_value[3], buf);

    if (s_sub_label) {
        char sub[96];
        snprintf(sub, sizeof(sub), "最后开启 · %s　·　开锁方式 %s", a->last_open, a->methods);
        lv_label_set_text(s_sub_label, sub);
    }

    for (int i = 0; i < EV_ROWS; i++) {
        lv_obj_t * r = s_rows[i].box ? lv_obj_get_parent(s_rows[i].box) : NULL;
        if (i < a->ev_n) {
            icon_label_set_glyph(s_rows[i].icon, a->ev[i].glyph);
            lv_obj_set_style_text_color(s_rows[i].icon, theme_color(a->ev[i].role), 0);
            lv_label_set_text(s_rows[i].title, a->ev[i].title);
            lv_label_set_text(s_rows[i].sub, a->ev[i].sub);
            lv_label_set_text(s_rows[i].time, a->ev[i].time);
            if (r) lv_obj_set_hidden(r, false);
        } else {
            lv_label_set_text(s_rows[i].title, "");
            lv_label_set_text(s_rows[i].sub, "");
            lv_label_set_text(s_rows[i].time, "");
            if (r) lv_obj_set_hidden(r, true);
        }
    }
}

static void monitor_timer_cb(lv_timer_t * t)
{
    (void)t;
    apply_local_colors();       /* 锁状态可能已变化 */

    if (s_stats_busy) return;   /* 上一次统计还没回来，跳过本轮 */
    s_stats_busy = true;
    worker_post(stats_worker, &s_stats, stats_done);
}

/* 主题切换回调：重刷所有「按角色取色」的本地覆盖 */
static void monitor_refresh_theme(int idx)
{
    (void)idx;
    apply_local_colors();
    /* 事件行的状态色由 stats_done 按 role 重贴，这里补一次（避免切主题时
     * 在两次统计之间出现旧色） */
    for (int i = 0; i < EV_ROWS; i++) {
        if (s_rows[i].icon == NULL) continue;
        if (i < s_stats.ev_n) {
            lv_obj_set_style_text_color(s_rows[i].icon, theme_color(s_stats.ev[i].role), 0);
        }
    }
}

/* 一键开锁：优先走无感的人脸通道；无人录入人脸时退回 PIN 键盘 */
static void fast_unlock_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(s_stats.face_count > 0 ? PAGE_FACE : PAGE_KEYPAD);
}

/* 密码开锁：PIN 键盘页（PIN 通道必须始终可达） */
static void pin_unlock_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_KEYPAD);
}

