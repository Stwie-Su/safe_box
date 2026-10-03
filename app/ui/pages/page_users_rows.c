/**
 * @file page_users_rows.c
 * 用户管理 — 列表与行渲染（D24 自 page_users.c 拆出，纯搬移）：
 * 角色分组头 / 用户行 / 行内操作按钮 / 名称缓存 / 列表重建 /
 * EV_USER_CHANGED 订阅回调；行回调（row_*_cb）设置 s_sel_id 后转调
 * page_users_dlg.c 的入口。
 */
#include "page_users.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/icons.h"               /* icon_label_colored：图标一律走图标字体 + theme token */
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "core/event_bus.h"
#include "core/auth/unlock_backend.h"   /* backend_admin_verify_totp：改管理员密码动态码二次确认 */
#include "hal/hal_face.h"               /* face_service_enroll/delete_async + caps（UI 现代化 ui3） */
#include "hal/hal_time.h"               /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include "ui/pages/page_enroll.h"       /* page_enroll_show_result：录入结果页内展示 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "page_users_internal.h"

/* ---- 文件内前向声明：add_user_row 以函数指针引用行回调，定义在本文件尾部 ---- */
static void row_pwd_cb(lv_event_t * e);
static void row_tog_cb(lv_event_t * e);
static void row_del_cb(lv_event_t * e);
static void row_face_cb(lv_event_t * e);

/* 行构建时缓存 id→显示名，供「修改信息」弹窗预填当前用户名。
 * 仅读内存、不直连 store，满足 QA-20 主线程门禁（禁止 UI 线程直接调 store 读盘）。 */
#define NAME_CACHE_SZ 128
static int   s_nc_id[NAME_CACHE_SZ];
static char  s_nc_name[NAME_CACHE_SZ][32];
static int   s_nc_cnt = 0;
static void name_cache_put(int id, const char *nm)
{
    int slot = -1;
    for (int i = 0; i < s_nc_cnt; i++) if (s_nc_id[i] == id) { slot = i; break; }
    if (slot < 0 && s_nc_cnt < NAME_CACHE_SZ) slot = s_nc_cnt++;
    if (slot >= 0) {
        s_nc_id[slot] = id;
        strncpy(s_nc_name[slot], nm, sizeof(s_nc_name[0]) - 1);
        s_nc_name[slot][sizeof(s_nc_name[0]) - 1] = '\0';
    }
}
const char * name_cache_get(int id)
{
    for (int i = 0; i < s_nc_cnt; i++) if (s_nc_id[i] == id) return s_nc_name[i];
    return NULL;
}

bool face_cap_ok(bool need_enroll)
{
    const face_caps_t * caps = face_service_caps();
    if (caps == NULL) return false;
    uint32_t need = need_enroll ? FACE_CAP_ENROLL : FACE_CAP_DELETE;
    return (caps->caps & need) != 0u;
}

/* ================================================================
 *  页面构建 — Linux 风格紧凑表格
 *  ┌──────────────────────────────────────────────────────────┐
 *  │ ‹ 返回    用户管理                        [+ 添加用户]  │
 *  ├─────────┬────────┬──────────┬────────┬─────────────────┤
 *  │ 用户名   │ 角色   │ 状态     │ 操作   │                 │
 *  ├─────────┼────────┼──────────┼────────┼─────────────────┤
 *  │ 👤admin │ 管理员  │ ● 启用   │ 改密 ⋯ │ ← 每行一个用户   │
 *  └─────────┴────────┴──────────┴────────┴─────────────────┘
 * ================================================================ */

/* ================================================================
 *  行渲染 — FR-28：用户列表按角色分区（管理员 / 普通用户 / 临时用户）
 *
 *  分区表：顺序即展示顺序（权限由高到低），与 store 的角色字符串一一对应。
 *  组内按 id 升序（= 创建顺序），同一份数据多次进页面顺序不变（规约 §5.8「稳定」）。
 *  未落在三张表里的角色（理论上不存在，store 只允许这三种）归入「普通用户」，
 *  保证任何数据都不会在界面上凭空消失。
 * ================================================================ */
typedef struct {
    const char * key;     /* store 里的 role 字符串 */
    const char * title;   /* 分组标题 */
} role_group_t;

static const role_group_t GROUPS[3] = {
    { "admin", "管理员"   },
    { "user",  "普通用户" },
    { "temp",  "临时用户" },
};
#define GROUP_CNT 3

/* 分组标题：复用 page_logs.c 的日期分组头版式（透明底 + 底部 1px 分隔线 +
 * 左右两端对齐的「标题 / 计数」），不另造一套视觉语言。 */
static void add_group_header(lv_obj_t * parent, const char * title, int count)
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
    lv_obj_set_style_pad_left(h, SX(4), 0);
    lv_obj_set_style_pad_right(h, SX(4), 0);
    lv_obj_set_style_pad_top(h, SY(12), 0);
    lv_obj_set_style_pad_bottom(h, SY(9), 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(h, false);

    lv_obj_t * tl = lv_label_create(h);
    lv_label_set_text(tl, title);
    lv_obj_add_style(tl, &st_text, 0);
    lv_obj_set_style_text_font(tl, app_font_scaled(15), 0);

    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d 人", count);
    lv_obj_t * cl = lv_label_create(h);
    lv_label_set_text(cl, cbuf);
    lv_obj_add_style(cl, &st_text_mut, 0);
    lv_obj_set_style_text_font(cl, app_font_scaled(13), 0);
}

/* 临时用户行副文案（规约 §5.8 交由实现决定 —— 此处选择展示）：
 * 临时授权的全部意义就是「还剩多少」，不展示的话管理员无法判断某个房客是否快到期。
 * 两项都没设（不限时不限次）时也要写出来，否则与「读取失败」无法区分。 */
static void fmt_temp_sub(const safe_user_t * u, char * out, size_t cap)
{
    char left[32]  = {0};
    char until[32] = {0};

    if (u->use_limit > 0) {
        int rem = u->use_limit - u->used_count;
        if (rem < 0) rem = 0;
        snprintf(left, sizeof(left), "剩 %d/%d 次", rem, u->use_limit);
    }
    if (u->valid_until > 0) {
        time_t t = (time_t)u->valid_until;
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(until, sizeof(until), "至 %m-%d %H:%M", &tmv);
    }

    if (left[0] && until[0]) snprintf(out, cap, "%s · %s", left, until);
    else if (left[0])        snprintf(out, cap, "%s", left);
    else if (until[0])       snprintf(out, cap, "%s", until);
    else                     snprintf(out, cap, "不限时 · 不限次");

    /* 已过期但尚未被自动删除（FR-9 的删除发生在「任一通道命中时」）：显式标注，
     * 免得管理员以为这条授权还能用。 */
    if (u->valid_until > 0 && (int64_t)hal_time() >= u->valid_until) {
        char tmp[72];
        snprintf(tmp, sizeof(tmp), "%s · 已失效", out);
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = '\0';
    }
}

/* 行内操作按钮：统一「图标 + 短文案」，四项操作同层同高 —— 原来 改密/删除 只有
 * 图标、停用/人脸 只有文字，是典型的图标文字混排，扫读成本高（信息架构层面）。
 * 图标走 icons.c 的图标字体 + theme token，颜色随主题联动。 */
static lv_obj_t * row_op_btn(lv_obj_t * parent, ui_glyph_t glyph, const char * text,
                             int32_t w, lv_style_t * style, theme_role_t ink,
                             lv_event_cb_t cb, int uid)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_set_size(b, w, SY(52));
    lv_obj_add_style(b, style, 0);
    lv_obj_set_style_pad_hor(b, SX(6), 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_pad_column(b, SX(4), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * ic = icon_label_colored(b, glyph, 16, ink);
    (void)ic;

    lv_obj_t * lb = lv_label_create(b);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, app_font_scaled(13), 0);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    return b;
}

/* 单个用户行：[头像圆] [用户名(+临时副文案)] [弹性占位] [状态] [操作组] */
static void add_user_row(const safe_user_t * u, bool is_admin, bool last_admin)
{
    int uid = u->id;

    /* 缓存 id→name，供修改信息弹窗预填（主线程读内存，不直连 store） */
    name_cache_put(u->id, u->name);

    /* 行容器 */
    lv_obj_t * row = lv_obj_create(s_list);
    lv_obj_set_size(row, lv_pct(100), SY(72));
    lv_obj_add_style(row, &st_panel2, 0);
    lv_obj_set_style_radius(row, SX(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, SX(8), 0);
    lv_obj_set_style_pad_row(row, 0, 0);
    /* st_panel2 默认 pad_all=10：上下压到各 4，行高 72 → 64 可用，
     * 操作按钮 52 与「用户名 + 临时副文案」两行（≈42px）都放得下。 */
    lv_obj_set_style_pad_ver(row, SY(4), 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);  /* 防止行内滚动 */

    /* --- 左侧：头像圆 --- */
    /* ★ 用直径 28 的小圆代替「▸」字符（Noto CJK 没收录 ▸）：纯色块无字形依赖；
     *      admin 用 accent 强调色，普通用户用 panel2（次面板）边框色。
     * ★★ 必须 lv_obj_set_scrollable(icon, false) —— LVGL v9 的 lv_obj 默认可滚动
     *      （lv_button 在构造里自己关掉了，lv_obj 不会），不关的话这个圆能被拖动
     *      并渲染出一条滚动条。这就是本页头像区 scrollbar 的根因；
     *      同页的 sp1 / sp2 弹性占位是同一个疏漏，一并关掉。 */
    lv_obj_t * icon = lv_obj_create(row);
    lv_obj_set_size(icon, SX(40), SX(40));
    lv_obj_set_scrollable(icon, false);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    if (is_admin) {
        lv_obj_set_style_bg_color(icon, theme_color(TH_ACCENT), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
    } else {
        lv_obj_set_style_bg_color(icon, theme_color(TH_PANEL2), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(icon, theme_color(TH_TEXT_MUT), 0);
        lv_obj_set_style_border_width(icon, 1, 0);
    }
    /* 圆里放用户首字母（首字符是字母数字，font 一定有） */
    char initial[4] = { u->name[0] ? u->name[0] : '?', '\0' };
    lv_obj_t * initial_lbl = lv_label_create(icon);
    lv_label_set_text(initial_lbl, initial);
    lv_obj_set_style_text_font(initial_lbl, app_font_scaled(16), 0);
    lv_obj_set_style_text_color(initial_lbl, is_admin ? theme_color(TH_ACCENT_INK) : theme_color(TH_TEXT), 0);
    lv_obj_center(initial_lbl);

    /* --- 用户名（+ 临时用户副文案） --- */
    lv_obj_t * mid = lv_obj_create(row);
    lv_obj_set_size(mid, SX(190), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_pad_row(mid, SY(1), 0);
    lv_obj_set_style_pad_column(mid, 0, 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(mid, false);

    lv_obj_t * nm = lv_label_create(mid);
    lv_label_set_text(nm, u->name);
    lv_obj_add_style(nm, &st_text, 0);
    lv_obj_set_style_text_font(nm, app_font_scaled(17), 0);
    lv_obj_set_width(nm, SX(190));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);   /* 超长用户名截断，不撑破行高 */

    if (strcmp(u->role, "temp") == 0) {
        char sub[72];
        fmt_temp_sub(u, sub, sizeof(sub));
        lv_obj_t * sl = lv_label_create(mid);
        lv_label_set_text(sl, sub);
        lv_obj_add_style(sl, &st_text_mut, 0);
        lv_obj_set_style_text_font(sl, app_font_scaled(11), 0);
        lv_obj_set_width(sl, SX(190));
        lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
    }

    /* 弹性占位，把状态与操作组推到右侧 */
    lv_obj_t * sp1 = lv_obj_create(row);
    lv_obj_set_flex_grow(sp1, 1);
    /* ★ D25：同 page_users.c::spacer_h —— 不给高度就是 LV_DPI_DEF(130)，
     * 把行（SIZE_CONTENT）撑到 170+，列表区被挤成一条。 */
    lv_obj_set_height(sp1, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sp1, 0, 0);
    lv_obj_set_style_bg_opa(sp1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp1, 0, 0);
    lv_obj_set_scrollable(sp1, false);

    /* --- 状态指示器 --- */
    lv_obj_t * st_lbl = lv_label_create(row);
    if (u->enabled) {
        lv_label_set_text(st_lbl, "● 启用");  /* ● 启用 */
        lv_obj_add_style(st_lbl, &st_ok_text, 0);
    } else {
        lv_label_set_text(st_lbl, "● 停用");  /* ● 停用 */
        lv_obj_add_style(st_lbl, &st_warn_text, 0);
    }
    lv_obj_set_style_text_font(st_lbl, app_font_scaled(13), 0);
    lv_obj_set_width(st_lbl, SX(72));

    /* 小间距 */
    lv_obj_t * sp2 = lv_obj_create(row);
    lv_obj_set_size(sp2, SX(8), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sp2, 0, 0);   /* ★ D25：清掉默认 pad 的 40 高 */
    lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);
    lv_obj_set_scrollable(sp2, false);

    /* --- 右侧操作组：修改信息 / 启停 / 人脸 / 删除 --- */
    lv_obj_t * ops = lv_obj_create(row);
    lv_obj_set_size(ops, LV_SIZE_CONTENT, SY(52));
    lv_obj_set_style_bg_opa(ops, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ops, 0, 0);
    lv_obj_set_style_pad_all(ops, 0, 0);
    lv_obj_set_style_pad_column(ops, SX(8), 0);
    lv_obj_set_flex_flow(ops, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ops, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ops, false);

    /* 修改信息（改名 + 可选改 PIN） */
    row_op_btn(ops, UI_GLYPH_EDIT, "修改信息", SX(84), &st_ghost_btn, TH_TEXT,
               row_pwd_cb, uid);

    /* 启用/停用 —— 末位管理员锁死 */
    lv_obj_t * b_tog = row_op_btn(ops,
                                  u->enabled ? UI_GLYPH_LOCK : UI_GLYPH_LOCK_OPEN,
                                  u->enabled ? "停用" : "启用",
                                  SX(84), &st_ghost_btn, TH_TEXT, NULL, uid);
    if (last_admin) {
        lv_obj_add_state(b_tog, LV_STATE_DISABLED);
        lv_obj_set_style_opa(b_tog, LV_OPA_40, LV_STATE_DISABLED);
    } else {
        lv_obj_add_event_cb(b_tog, row_tog_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    }

    /* 人脸 —— 已录入（face_id>=0）显示「删人脸」，未录入显示「录人脸」。
     * 后端不支持录入/删除时整颗按钮置灰（SAFE_FACE_BACKEND=none）。 */
    bool has_face = (u->face_id >= 0);
    bool cap_ok = face_cap_ok(!has_face);
    lv_obj_t * b_face = row_op_btn(ops, UI_GLYPH_FACE, has_face ? "删人脸" : "录人脸",
                                   SX(92), &st_ghost_btn,
                                   has_face ? TH_DANGER : TH_ACCENT, NULL, uid);
    if (!cap_ok) {
        lv_obj_add_state(b_face, LV_STATE_DISABLED);
        lv_obj_set_style_opa(b_face, LV_OPA_40, LV_STATE_DISABLED);
    } else {
        lv_obj_add_event_cb(b_face, row_face_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    }

    /* 删除 —— 末位管理员锁死 */
    lv_obj_t * b_del = row_op_btn(ops, UI_GLYPH_DELETE, "删除", SX(84), &st_danger_btn,
                                  TH_ACCENT_INK, NULL, uid);
    if (last_admin) {
        lv_obj_add_state(b_del, LV_STATE_DISABLED);
        lv_obj_set_style_opa(b_del, LV_OPA_40, LV_STATE_DISABLED);
    } else {
        lv_obj_add_event_cb(b_del, row_del_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    }
}

static void users_list_loaded(safe_user_t * us, int n)
{
    lv_obj_clean(s_list);
    s_nc_cnt = 0;   /* 重建列表前清空 id→name 缓存 */

    /* 空态：store 保证至少有一个管理员，理论上到不了这里；真到了也要说清
     * 「下一步该做什么」，而不是留一片空白（信息架构里的空态提示）。 */
    if (n <= 0) {
        /* S1：区分「读取失败」与「真的没有用户」。
         * 两者的回调都是 (NULL, 0)，必须查 async_store 记录的失败标志 ——
         * 否则读取失败会被显示成空态，还诱导用户在数据损坏时点「添加用户」，
         * 那会把损坏的文件覆盖掉。 */
        const bool load_failed = astore_users_load_failed();
        if (s_add_btn) {
            if (load_failed) lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);
            else             lv_obj_remove_state(s_add_btn, LV_STATE_DISABLED);
        }
        if (s_summary) {
            lv_label_set_text(s_summary, load_failed ? "用户数据读取失败"
                                                      : "共 0 位用户");
        }
        lv_obj_t * em = lv_label_create(s_list);
        lv_label_set_text(em, load_failed
                               ? "读取失败：存储异常 —— 已暂时禁用「添加用户」，\n"
                                 "以免覆盖损坏的数据"
                               : "暂无用户 —— 点右上角「添加用户」创建");
        lv_obj_add_style(em, &st_text_mut, 0);
        lv_obj_set_style_text_font(em, app_font_scaled(14), 0);
        lv_obj_set_width(em, lv_pct(100));
        lv_obj_set_style_text_align(em, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(em, SY(28), 0);
        return;
    }

    /* ★ 先统计「启用中的管理员」数量：为 1 时该管理员的停用/删除必须锁死，
     * 否则设备将进入无人可管理的死锁状态（没有任何账号能再进设置页）。 */
    int admin_enabled = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].role, "admin") == 0 && us[i].enabled) admin_enabled++;
    }

    /* 分区下标表：order[g] 存第 g 组的用户下标，组内按 id 升序插入（稳定排序） */
    int * order[GROUP_CNT];
    int   cnt[GROUP_CNT] = {0, 0, 0};
    int * pool = (int *)calloc((size_t)n * GROUP_CNT, sizeof(int));
    if (!pool) return;
    for (int g = 0; g < GROUP_CNT; g++) order[g] = pool + (size_t)g * (size_t)n;

    for (int i = 0; i < n; i++) {
        int g = 1;                               /* 默认归入「普通用户」 */
        for (int k = 0; k < GROUP_CNT; k++) {
            if (strcmp(us[i].role, GROUPS[k].key) == 0) { g = k; break; }
        }
        /* 插入排序：等长、无 qsort 比较器副作用，且对「多次进页面顺序一致」是
         * 确定性的（相同 id 只可能出现一次，不存在不稳定情形）。 */
        int p = cnt[g];
        while (p > 0 && us[order[g][p - 1]].id > us[i].id) {
            order[g][p] = order[g][p - 1];
            p--;
        }
        order[g][p] = i;
        cnt[g]++;
    }

    if (s_summary) {
        char sum[80];
        snprintf(sum, sizeof(sum), "共 %d 位用户 · 管理员 %d · 普通 %d · 临时 %d",
                 n, cnt[0], cnt[1], cnt[2]);
        lv_label_set_text(s_summary, sum);
    }

    /* 空分区不显示（规约 §5.8 明确交由实现决定）：三张标题全铺开会让「只有 1 个
     * 管理员」的新设备出现两条 0 人标题，纯噪声；构成信息已由顶部统计行承载。 */
    for (int g = 0; g < GROUP_CNT; g++) {
        if (cnt[g] <= 0) continue;
        add_group_header(s_list, GROUPS[g].title, cnt[g]);
        for (int j = 0; j < cnt[g]; j++) {
            const safe_user_t * u = &us[order[g][j]];
            bool is_admin   = (strcmp(u->role, "admin") == 0);
            bool last_admin = (is_admin && u->enabled && admin_enabled <= 1);
            add_user_row(u, is_admin, last_admin);
        }
    }

    free(pool);
}

void rebuild_list(void)
{
    astore_load_users(users_list_loaded);
}

/* ---- 行操作回调：设置 s_sel_id 再调用原有逻辑 ---- */
static void row_pwd_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    pwd_btn_cb(e);
}

static void row_tog_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    toggle_btn_cb(e);
}

static void row_del_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    del_btn_cb(e);
}

/* 行内「录/删人脸」按钮：记下操作类型后进 admin PIN 二次确认 */
static void row_face_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    face_btn_cb(e);
}


void on_user_changed(ev_topic_t topic, const void * payload, void * user)
{
    (void)payload; (void)user;
    if (topic != EV_USER_CHANGED) return;
    astore_load_users(users_list_loaded);
}

