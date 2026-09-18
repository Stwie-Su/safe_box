/**
 * @file page_logs.c
 * 操作日志页（v2 卡片化；视觉真源 ui_redesign_preview_v2.html 的 #sc-logs）。
 *
 * 布局（内容区 920×544 下）：
 *   1) 标题区：标题「操作日志」+ 副标题（**真实**总数 / 今日条数，走 log_query）
 *   2) 筛选栏：5 个 chips（全部 / 开锁 / 用户 / 设置 / 失败），选中态 = accent 实心 +
 *      accent ink 文字；单击【就地重建列表】（不新建页面、不清空重进）
 *   3) 列表面板：日期分组头（日期 + 该日条数）+ 行
 *      （类型着色圆角图标块 + 主文案 + 灰色副文案 + 右侧等宽时间列）
 *
 * 数据源：core/store 的 safe.log（JSON Lines），经 async_store 在 worker 线程读取，
 * 主线程 worker_poll() 回调里渲染（DESIGN.md §9）。
 *
 * ⚠️ 大小写铁律：log_query() / store.c 的 evt 过滤是**精确 strcmp**（store.c:1020）。
 *    写入侧实际使用的字符串（grep log_append 全量核对）为：
 *      "UNLOCK" / "DENY" / "LOCKOUT" / "ALARM"           （auth_fsm.c，全大写）
 *      "user_add" / "user_del" / "user_modify"           （page_users.c）
 *      "pwd_change" / "setting_change" / "factory_reset" （page_users.c / store.c / page_system.c）
 *    历史缺陷：旧代码查小写 "unlock"，与写入的 "UNLOCK" 不匹配，导致「最后开启」恒为
 *    "--"（page_monitor.c 已修正）。本页 EVT_META 表与写入侧逐字一致。
 *
 * 性能与纪律：
 *   - 每 2s 轮询一次（日志为追加型）；用「数据签名」（条数 + 首尾时间戳）比较，
 *     未变化则跳过 DOM 重建——既省 A7 重绘，又保住滚动位置；
 *   - 主题切换：全工程 theme_change_cb 槽位（THEME_CB_MAX=4）已被 topbar / rail /
 *     monitor / face 占满，本页无法再注册；改为在轮询里比较 theme_idx()，变化即
 *     刷新本地颜色并强制重建列表（页面隐藏时也在跑，切回时已是新配色）；
 *   - 颜色 100% 走 theme_color(TH_*) / st_* 样式，零硬编码 hex（CLAUDE.md §8）。
 */
#include "page_logs.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "hal/hal_time.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/* ==================== 常量 ==================== */
#define LOGS_MAX_ROWS    200    /* 单次渲染最大行数（防超长列表卡顿，同旧版上限） */
#define LOGS_POLL_MS     2000   /* 日志轮询周期（追加型，2s 足够；签名未变不重建） */
#define LOGS_TIME_COL_W  74     /* 右侧等宽时间列宽（设计稿 px） */

/* ==================== 筛选器 ==================== */
typedef enum {
    FIL_ALL = 0,   /* 全部 */
    FIL_UNLOCK,    /* 开锁：UNLOCK */
    FIL_USER,      /* 用户：user_* */
    FIL_SETTING,   /* 设置：setting_change / pwd_change / factory_reset */
    FIL_FAIL,      /* 失败：res == 0（任意事件） */
    FIL_COUNT
} log_filter_t;

static const char * const FIL_LABELS[FIL_COUNT] = { "全部", "开锁", "用户", "设置", "失败" };

/* ============ 事件类型 → 图标 / 颜色 / 中文名（唯一映射表，便于以后加类型） ============
 * evt 必须与写入侧逐字一致（大小写敏感）。新增事件类型在此追加一行即可。 */
typedef struct {
    const char * evt;    /* 写入侧事件名（store.c log_append） */
    ui_glyph_t   glyph;  /* 图标字体码位 */
    theme_role_t role;   /* 着色 token */
    const char * title;  /* 主文案 */
} evt_meta_t;

static const evt_meta_t EVT_META[] = {
    { "UNLOCK",         UI_GLYPH_FINGER,   TH_OK,     "开锁成功"       },
    { "DENY",           UI_GLYPH_LOCK,     TH_DANGER, "开锁被拒"       },
    { "LOCKOUT",        UI_GLYPH_SHIELD,   TH_DANGER, "连续失败已锁定" },
    { "ALARM",          UI_GLYPH_WARN,     TH_WARN,   "安防告警"       },
    { "user_add",       UI_GLYPH_PERSON,   TH_ACCENT, "新增用户"       },
    { "user_del",       UI_GLYPH_DELETE,   TH_ACCENT, "删除用户"       },
    { "user_modify",    UI_GLYPH_EDIT,     TH_ACCENT, "修改用户"       },
    { "pwd_change",     UI_GLYPH_KEY,      TH_ACCENT, "修改密码"       },
    { "setting_change", UI_GLYPH_SETTINGS, TH_TEXT,   "设置变更"       },
    { "factory_reset",  UI_GLYPH_UNDO,     TH_WARN,   "恢复出厂设置"   },
};
#define EVT_META_N ((int)(sizeof(EVT_META) / sizeof(EVT_META[0])))

/* ==================== 行视图 ==================== */
typedef struct {
    ui_glyph_t   glyph;
    theme_role_t role;
    char title[40];
    char sub[80];
    char time[12];   /* HH:MM:SS */
} log_row_view_t;

/* ==================== 静态对象 ==================== */
static lv_obj_t * s_list;                       /* 列表面板 */
static lv_obj_t * s_sum_label;                  /* 副标题：共 N 条 · 今日 M 条 */
static lv_obj_t * s_filter_btn[FIL_COUNT];      /* 筛选 chips */
static lv_obj_t * s_filter_lbl[FIL_COUNT];      /* chips 文本（选中态改色） */

static log_filter_t s_filter = FIL_ALL;

/* 数据签名：条数 + 首尾时间戳。未变化且非强制时跳过 DOM 重建。 */
static int  s_last_n = -1;
static char s_last_head[24];
static char s_last_tail[24];
static bool s_force_rebuild = false;
static int  s_theme_idx = -1;

/* ==================== 私有声明 ==================== */
static void build_header(lv_obj_t * parent);
static void build_filter_bar(lv_obj_t * parent);
static void build_list(lv_obj_t * parent);
static void filter_click_cb(lv_event_t * e);
static void logs_timer_cb(lv_timer_t * t);
static void rebuild_list(void);
static void apply_chip_style(void);
static void refresh_local_colors(void);
static void logs_loaded(log_entry_t * all, int n);
static const evt_meta_t * evt_meta_lookup(const char * evt);
static void describe_log(const log_entry_t * e, log_row_view_t * v);
static bool filter_match(const log_entry_t * e);
static void add_group_header(lv_obj_t * parent, const char * date, int count, bool is_today);
static void add_row(lv_obj_t * parent, const log_entry_t * e);
static lv_obj_t * mk_row_base(lv_obj_t * parent);
static lv_obj_t * mk_icon_box(lv_obj_t * parent, int32_t size, int32_t radius, theme_role_t role);
static void today_str(char * out, size_t cap);

/* ================================================================
 *  页面构建
 * ================================================================ */
lv_obj_t * page_logs_create(lv_obj_t * parent)
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
    lv_obj_set_style_pad_row(root, SY(12), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(root, false);

    build_header(root);
    build_filter_bar(root);
    build_list(root);

    s_theme_idx = theme_idx();

    /* 周期刷新（追加型日志；签名未变时回调内直接跳过重建） */
    lv_timer_create(logs_timer_cb, LOGS_POLL_MS, NULL);
    rebuild_list();
    return root;
}

/* 标题区：标题 + 副标题（副标题初值留空，logs_loaded 后填真实统计） */
static void build_header(lv_obj_t * parent)
{
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_outline_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_row(head, SY(3), 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "操作日志");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(24), 0);

    s_sum_label = lv_label_create(head);
    lv_label_set_text(s_sum_label, "");
    lv_obj_add_style(s_sum_label, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_sum_label, app_font_scaled(13), 0);
}

/* 筛选栏：5 个 pill chips */
static void build_filter_bar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_outline_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, SX(8), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);

    for (int i = 0; i < FIL_COUNT; i++) {
        lv_obj_t * chip = lv_button_create(bar);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, SY(30));
        lv_obj_set_style_radius(chip, SY(15), 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_outline_width(chip, 0, 0);
        lv_obj_set_style_pad_left(chip, SX(14), 0);
        lv_obj_set_style_pad_right(chip, SX(14), 0);
        lv_obj_set_style_pad_top(chip, 0, 0);
        lv_obj_set_style_pad_bottom(chip, 0, 0);

        lv_obj_t * lbl = lv_label_create(chip);
        lv_label_set_text(lbl, FIL_LABELS[i]);
        lv_obj_set_style_text_font(lbl, app_font_scaled(13), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(chip, filter_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_filter_btn[i] = chip;
        s_filter_lbl[i] = lbl;
    }
    apply_chip_style();
}

/* 列表面板 */
static void build_list(lv_obj_t * parent)
{
    s_list = lv_obj_create(parent);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_add_style(s_list, &st_panel, 0);
    lv_obj_set_style_radius(s_list, SX(16), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);                 /* 层次：1px TH_BORDER，不用阴影 */
    lv_obj_set_style_border_color(s_list, theme_color(TH_BORDER), 0);
    lv_obj_set_style_pad_left(s_list, SX(14), 0);
    lv_obj_set_style_pad_right(s_list, SX(14), 0);
    lv_obj_set_style_pad_top(s_list, SY(4), 0);
    lv_obj_set_style_pad_bottom(s_list, SY(8), 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
}

/* ================================================================
 *  筛选 chips
 * ================================================================ */

/* 选中 = accent 实心 + accent ink 文字；未选 = panel2 + 次要文字 */
static void apply_chip_style(void)
{
    for (int i = 0; i < FIL_COUNT; i++) {
        if (s_filter_btn[i] == NULL) continue;
        bool on = (i == (int)s_filter);
        lv_obj_set_style_bg_color(s_filter_btn[i],
                                  theme_color(on ? TH_ACCENT : TH_PANEL2), 0);
        lv_obj_set_style_bg_opa(s_filter_btn[i], LV_OPA_COVER, 0);
        if (s_filter_lbl[i] != NULL) {
            lv_obj_set_style_text_color(s_filter_lbl[i],
                                        theme_color(on ? TH_ACCENT_INK : TH_TEXT_MUT), 0);
        }
    }
}

/* 单击就地重建列表（不新建页面、不清空重进） */
static void filter_click_cb(lv_event_t * e)
{
    log_filter_t f = (log_filter_t)(uintptr_t)lv_event_get_user_data(e);
    if (f == s_filter) return;
    s_filter = f;
    apply_chip_style();
    s_force_rebuild = true;    /* 筛选变化必须重建（签名未变也要重建） */
    rebuild_list();
}

/* ================================================================
 *  渲染
 * ================================================================ */

/* 类型 → 元数据（未登记的走兜底：铃铛 + 次要文字 + 原始事件名） */
static const evt_meta_t * evt_meta_lookup(const char * evt)
{
    for (int i = 0; i < EVT_META_N; i++) {
        if (strcmp(evt, EVT_META[i].evt) == 0) return &EVT_META[i];
    }
    return NULL;
}

/* 单条日志 → 行视图（图标 / 颜色 / 主文案 / 副文案 / 时间） */
static void describe_log(const log_entry_t * e, log_row_view_t * v)
{
    const evt_meta_t * m = evt_meta_lookup(e->evt);
    if (m != NULL) {
        v->glyph = m->glyph;
        v->role  = m->role;
        snprintf(v->title, sizeof(v->title), "%s", m->title);
    } else {
        v->glyph = UI_GLYPH_BELL;
        v->role  = TH_TEXT_MUT;
        snprintf(v->title, sizeof(v->title), "%s", e->evt);
    }

    /* 副文案：用户 · 详情（"-" 视为无用户） */
    const char * u = (e->user[0] != '\0' && strcmp(e->user, "-") != 0) ? e->user : NULL;
    const char * d = (e->detail[0] != '\0') ? e->detail : NULL;
    if (u != NULL && d != NULL)      snprintf(v->sub, sizeof(v->sub), "%s · %s", u, d);
    else if (d != NULL)              snprintf(v->sub, sizeof(v->sub), "%s", d);
    else if (u != NULL)              snprintf(v->sub, sizeof(v->sub), "%s", u);
    else                             v->sub[0] = '\0';

    /* 时间列：ISO 时间戳第 12~19 位 = HH:MM:SS（等宽右对齐由布局保证） */
    if (strlen(e->ts) >= 19)       snprintf(v->time, sizeof(v->time), "%.8s", e->ts + 11);
    else if (strlen(e->ts) >= 16)  snprintf(v->time, sizeof(v->time), "%.5s", e->ts + 11);
    else                           snprintf(v->time, sizeof(v->time), "--:--:--");
}

/* 当前筛选是否命中某条日志（"失败"只看 res，其余看事件类型） */
static bool filter_match(const log_entry_t * e)
{
    switch (s_filter) {
        case FIL_ALL:
            return true;
        case FIL_UNLOCK:
            return strcmp(e->evt, "UNLOCK") == 0;
        case FIL_USER:
            return strncmp(e->evt, "user_", 5) == 0;
        case FIL_SETTING:
            return strcmp(e->evt, "setting_change") == 0 ||
                   strcmp(e->evt, "pwd_change") == 0 ||
                   strcmp(e->evt, "factory_reset") == 0;
        case FIL_FAIL:
            return e->res == 0;
        default:
            return true;
    }
}

/* 行底板：全宽、透明底、底部 1px 分隔（层次靠描边而非阴影） */
static lv_obj_t * mk_row_base(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, theme_color(TH_BORDER), 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_top(row, SY(7), 0);
    lv_obj_set_style_pad_bottom(row, SY(7), 0);
    lv_obj_set_style_pad_column(row, SX(12), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    return row;
}

/* 类型着色圆角图标块：底色 = token @20%，图标 = token（随主题刷新） */
static lv_obj_t * mk_icon_box(lv_obj_t * parent, int32_t size, int32_t radius, theme_role_t role)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, SX(size), SX(size));
    lv_obj_set_style_bg_color(box, theme_color(role), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_20, 0);
    lv_obj_set_style_radius(box, SX(radius), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

/* 日期分组头：左日期，右该日条数 */
static void add_group_header(lv_obj_t * parent, const char * date, int count, bool is_today)
{
    lv_obj_t * h = lv_obj_create(parent);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_height(h, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h, 1, 0);
    lv_obj_set_style_border_side(h, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(h, theme_color(TH_BORDER), 0);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_set_style_outline_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_set_style_pad_top(h, SY(9), 0);
    lv_obj_set_style_pad_bottom(h, SY(6), 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(h, false);

    lv_obj_t * dl = lv_label_create(h);
    lv_label_set_text(dl, date);
    lv_obj_add_style(dl, &st_text_mut, 0);
    lv_obj_set_style_text_font(dl, app_font_scaled(12), 0);

    char cbuf[24];
    if (is_today) snprintf(cbuf, sizeof(cbuf), "今日 %d 条", count);
    else          snprintf(cbuf, sizeof(cbuf), "%d 条", count);

    lv_obj_t * cl = lv_label_create(h);
    lv_label_set_text(cl, cbuf);
    lv_obj_add_style(cl, &st_text_mut, 0);
    lv_obj_set_style_text_font(cl, app_font_scaled(12), 0);
}

/* 一行日志：图标块 + （主文案 / 灰副文案）+ 等宽时间列 */
static void add_row(lv_obj_t * parent, const log_entry_t * e)
{
    log_row_view_t v;
    describe_log(e, &v);

    lv_obj_t * row = mk_row_base(parent);

    lv_obj_t * box = mk_icon_box(row, 34, 10, v.role);
    lv_obj_t * ic  = icon_label_colored(box, v.glyph, 18, v.role);
    lv_obj_center(ic);

    lv_obj_t * mid = lv_obj_create(row);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_outline_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_pad_row(mid, SY(1), 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(mid, false);

    lv_obj_t * ttl = lv_label_create(mid);
    lv_label_set_text(ttl, v.title);
    lv_obj_set_style_text_font(ttl, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(ttl, theme_color(TH_TEXT), 0);

    lv_obj_t * sub = lv_label_create(mid);
    lv_label_set_text(sub, v.sub);
    lv_obj_set_style_text_font(sub, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(sub, theme_color(TH_TEXT_MUT), 0);

    lv_obj_t * tm = lv_label_create(row);
    lv_obj_set_width(tm, SX(LOGS_TIME_COL_W));
    lv_label_set_text(tm, v.time);
    lv_obj_set_style_text_font(tm, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(tm, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_text_align(tm, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ================================================================
 *  异步查询回调（主线程）：统计 + 按需重建
 * ================================================================ */
static void logs_loaded(log_entry_t * all, int n)
{
    if (s_list == NULL) return;

    /* --- 副标题：真实总数 + 今日条数（与当前筛选无关） --- */
    char today[11];
    today_str(today, sizeof(today));
    int today_cnt = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(all[i].ts, today, 10) == 0) today_cnt++;
    }
    char sum[64];
    snprintf(sum, sizeof(sum), "共 %d 条 · 今日 %d 条", n, today_cnt);
    lv_label_set_text(s_sum_label, sum);

    /* --- 数据签名：未变化且非强制则跳过 DOM 重建 --- */
    bool changed = s_force_rebuild;
    if (!changed) {
        if (n != s_last_n) changed = true;
        else if (n > 0 && (strcmp(all[0].ts, s_last_head) != 0 ||
                           strcmp(all[n - 1].ts, s_last_tail) != 0)) changed = true;
    }
    s_force_rebuild = false;
    s_last_n = n;
    if (n > 0) {
        snprintf(s_last_head, sizeof(s_last_head), "%s", all[0].ts);
        snprintf(s_last_tail, sizeof(s_last_tail), "%s", all[n - 1].ts);
    } else {
        s_last_head[0] = '\0';
        s_last_tail[0] = '\0';
    }
    if (!changed) return;

    /* --- 重建：先按筛选收集（存指针，不拷贝），再按日期分组输出 --- */
    lv_obj_clean(s_list);

    const log_entry_t * sel[LOGS_MAX_ROWS];
    int ns = 0;
    for (int i = 0; i < n && ns < LOGS_MAX_ROWS; i++) {
        if (filter_match(&all[i])) sel[ns++] = &all[i];
    }

    if (ns == 0) {
        /* S1：区分「读取失败」与「确实没有符合条件的记录」——
         * 两者回调都是 (NULL, 0)，必须查 async_store 的失败标志。 */
        const bool load_failed = astore_logs_load_failed();
        lv_obj_t * hint = lv_label_create(s_list);
        lv_label_set_text(hint, load_failed
                                 ? "读取失败：存储异常，日志暂时无法显示"
                                 : "没有符合条件的记录");
        lv_obj_add_style(hint, &st_text_mut, 0);
        lv_obj_set_style_text_font(hint, app_font_scaled(14), 0);
        lv_obj_set_style_pad_top(hint, SY(12), 0);
        return;
    }

    int i = 0;
    while (i < ns) {
        char date[11];
        memcpy(date, sel[i]->ts, 10);
        date[10] = '\0';
        int j = i;
        while (j < ns && strncmp(sel[j]->ts, date, 10) == 0) j++;
        add_group_header(s_list, date, j - i, strcmp(date, today) == 0);
        for (int k = i; k < j; k++) add_row(s_list, sel[k]);
        i = j;
    }
    /* 列表数据由 astore 框架在回调返回后释放 */
}

/* ================================================================
 *  轮询 / 主题刷新
 * ================================================================ */
static void rebuild_list(void)
{
    astore_query_log(NULL, -1, logs_loaded);
}

/* 主题变化时刷新那些「按角色取色」的本地覆盖（chips / 列表描边） */
static void refresh_local_colors(void)
{
    apply_chip_style();
    if (s_list != NULL) {
        lv_obj_set_style_border_color(s_list, theme_color(TH_BORDER), 0);
    }
}

static void logs_timer_cb(lv_timer_t * t)
{
    (void)t;

    /* 本页无法注册 theme_change_cb（槽位已满，见文件头）；用 idx 比较替代 */
    int idx = theme_idx();
    if (idx != s_theme_idx) {
        s_theme_idx = idx;
        refresh_local_colors();
        s_force_rebuild = true;    /* 行内着色需重建（本地颜色是快照） */
    }
    rebuild_list();
}

/* ================================================================
 *  小工具
 * ================================================================ */
static void today_str(char * out, size_t cap)
{
    time_t now = (time_t)hal_time();
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, cap, "%Y-%m-%d", &tm_now);
}
