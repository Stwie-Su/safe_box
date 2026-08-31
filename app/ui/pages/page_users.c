/**
 * @file page_users.c
 * 用户管理 — Linux 风格紧凑表格布局（仿 /etc/passwd + users-admin）。
 * 每行显示：图标+用户名 | 角色标签 | 状态 | 内联操作按钮（改密/停用/删除）。
 * 数据经 core/store 落 users.json（PIN 只存 PBKDF2 哈希）。
 * 表单输入统一用 lv_keyboard 绑定当前 textarea。
 */
#include "page_users.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "core/auth/unlock_backend.h"   /* backend_admin_verify_totp：改管理员密码动态码二次确认 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 列表容器 */
static lv_obj_t * s_list = NULL;

/* 当前选中用户 ID（供弹窗回调使用） */
static int s_sel_id = -1;

/* 弹窗句柄 */
static lv_obj_t * s_ov  = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_kb  = NULL;
static lv_obj_t * s_msg = NULL;
/* ★ 弹窗内 textarea 句柄，按创建顺序存：
 *   idx 0: 用户名 / 旧 PIN  …
 *   idx 1: PIN / 新 PIN      ← 默认焦点（数字键盘）
 *   idx 2: 确认 PIN            */
static lv_obj_t * s_dlg_tas[3] = {NULL, NULL, NULL};

/* 管理员二次校验弹窗（删除用户前）：独立于 dlg_* 的自绘小键盘，
 * 不复用 lv_keyboard —— 避免与表单弹窗的 s_ov/s_kb 互相踩踏 */
static lv_obj_t * s_auth_ov   = NULL;
static lv_obj_t * s_auth_disp = NULL;
static lv_obj_t * s_auth_msg  = NULL;
static char s_auth_pin[16]    = {0};

/* 修改管理员密码 — 动态码二次确认（FR-3 敏感操作）状态 */
static bool s_pwd_is_admin    = false;   /* 当前改密目标是否为管理员 */
static char s_pending_old[16] = {0};     /* 暂存：管理员改密跨弹窗的 旧PIN */
static char s_pending_new[16] = {0};     /* 暂存：管理员改密跨弹窗的 新PIN */
static lv_obj_t * s_otp_ov    = NULL;
static lv_obj_t * s_otp_disp  = NULL;
static lv_obj_t * s_otp_msg   = NULL;
static char s_otp_pin[8]      = {0};

/* 键盘字号基准（实际字号 = base × ui_scale，见 ui_scale.c app_montserrat_scaled）
 * lv_keyboard 是 lv_buttonmatrix 子类，按键文本属于 LV_PART_ITEMS。
 *  字母/符号模式：板子 1.0x → 32px，PC 1.8x → 48px
 *  数字模式(3×4 大键)：板子 1.0x → 40px，PC 1.8x → 48px */
#define KB_FONT_BASE_TEXT    32
#define KB_FONT_BASE_NUMBER  40

/* ---- 前向声明 ---- */
static void rebuild_list(void);
static void go_back_cb(lv_event_t * e);
static void add_btn_cb(lv_event_t * e);
static void row_pwd_cb(lv_event_t * e);
static void row_tog_cb(lv_event_t * e);
static void row_del_cb(lv_event_t * e);
static void pwd_btn_cb(lv_event_t * e);
static void del_btn_cb(lv_event_t * e);
static void toggle_btn_cb(lv_event_t * e);

static void dlg_add_user(void);
static void dlg_change_pwd(void);
static void dlg_confirm_del(const char *name);
static void dlg_tip(const char *text);
static void close_dlg(void);
static lv_obj_t * dlg_open(const char *title, int w, int h);
static lv_obj_t * dlg_textarea(const char *label, bool password, bool number, int idx);
static void kb_focus_cb(lv_event_t * e);
static void save_add_cb(lv_event_t * e);
static void save_pwd_cb(lv_event_t * e);
static void do_del_cb(lv_event_t * e);
static void dlg_cancel_cb(lv_event_t * e);
static void dlg_ok_cb(lv_event_t * e);
static void dlg_set_msg(const char *text);

/* 敏感操作二次校验（管理员 PIN） */
static void auth_show(void);
static void auth_close(void);
static void auth_key_cb(lv_event_t * e);
static void auth_cancel_cb(lv_event_t * e);
static void auth_ok_cb(lv_event_t * e);
static void auth_result(int r);

/* 修改管理员密码 — 动态码二次确认（FR-3 敏感操作） */
static void pwd_otp_show(void);
static void pwd_otp_close(void);
static void pwd_otp_update_disp(void);
static void pwd_otp_key_cb(lv_event_t * e);
static void pwd_otp_ok_cb(lv_event_t * e);
static void pwd_otp_cancel_cb(lv_event_t * e);
static bool user_is_admin(int id);
static void post_chg_pwd(int id, const char *oldp, const char *newp);

/* 后台 worker 类型 */
typedef struct { char name[32]; char pin[16]; int result; } add_user_job_t;
typedef struct { int id; char oldpin[16]; char newpin[16]; int result; } chg_pwd_job_t;
typedef struct { int id; int result; } user_op_job_t;

static void users_list_loaded(safe_user_t * list, int count);
static void add_user_worker(void * p);
static void add_user_done(void * p);
static void chg_pwd_worker(void * p);
static void chg_pwd_done(void * p);
static void tog_worker(void * p);
static void tog_done(void * p);
static void del_check_loaded(safe_user_t * list, int count);
static void del_worker(void * p);
static void del_done(void * p);

static void salt_to_hex(const uint8_t salt[16], char out[33])
{
    static const char *HEX = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = HEX[salt[i] >> 4];
        out[i * 2 + 1] = HEX[salt[i] & 15];
    }
    out[32] = '\0';
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
lv_obj_t * page_users_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, SX(16), 0);
    lv_obj_set_style_pad_row(root, SY(8), 0);

    /* ------ 标题行 ------ */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * back = lv_button_create(head);
    lv_obj_set_size(back, SX(72), SY(38));
    lv_obj_add_style(back, &st_ghost_btn, 0);
    lv_obj_add_event_cb(back, go_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, "‹ 返回");  /* ‹ 返回 */
    lv_obj_set_style_text_font(bl, app_font_scaled(14), 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "用户管理");  /* 用户管理 */
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(24), 0);
    lv_obj_set_style_pad_left(title, SX(16), 0);

    lv_obj_t * spacer_h = lv_obj_create(head);
    lv_obj_set_flex_grow(spacer_h, 1);
    lv_obj_set_style_bg_opa(spacer_h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_h, 0, 0);

    lv_obj_t * add = lv_button_create(head);
    lv_obj_set_size(add, SX(110), SY(38));
    lv_obj_add_style(add, &st_accent_btn, 0);
    lv_obj_add_style(add, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(add, add_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(add);
    lv_label_set_text(al, "+ 添加用户");  /* + 添加用户 */
    lv_obj_set_style_text_font(al, app_font_scaled(14), 0);
    lv_obj_center(al);

    /* ------ 安全规则提示条 ------
     * 说明「为什么那一行按钮是灰的」，避免用户以为界面卡死。
     * 不用 st_text_mut（历史坑：该样式的 text_color 有时不级联到 label）→ 显式设色。 */
    lv_obj_t * rule = lv_label_create(root);
    lv_label_set_text(rule, "安全规则：系统必须保留至少 1 名「启用」状态的管理员");
    lv_obj_set_style_text_font(rule, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(rule, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_width(rule, lv_pct(100));

    /* ------ 表头 ------ */
    lv_obj_t * hdr = lv_obj_create(root);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_bottom(hdr, SY(6), 0);
    lv_obj_set_style_pad_row(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(hdr, SX(8), 0);

    /* 表头列 — 与行内对齐 */
    lv_obj_t * h_name = lv_label_create(hdr);
    lv_label_set_text(h_name, "用户名");  /* 用户名 */
    lv_obj_set_style_text_font(h_name, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_name, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_name, SX(160), LV_SIZE_CONTENT);

    lv_obj_t * h_role = lv_label_create(hdr);
    lv_label_set_text(h_role, "角色");  /* 角色 */
    lv_obj_set_style_text_font(h_role, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_role, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_role, SX(80), LV_SIZE_CONTENT);

    lv_obj_t * h_st = lv_label_create(hdr);
    lv_label_set_text(h_st, "状态");  /* 状态 */
    lv_obj_set_style_text_font(h_st, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_st, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_st, SX(80), LV_SIZE_CONTENT);

    lv_obj_t * h_sp = lv_obj_create(hdr);
    lv_obj_set_flex_grow(h_sp, 1);
    lv_obj_set_style_bg_opa(h_sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_sp, 0, 0);

    lv_obj_t * h_ops = lv_label_create(hdr);
    lv_label_set_text(h_ops, "操作");  /* 操作 */
    lv_obj_set_style_text_font(h_ops, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_ops, theme_color(TH_TEXT_MUT), 0);

    /* ------ 用户列表（全宽紧凑行） ------ */
    s_list = lv_obj_create(root);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_add_style(s_list, &st_panel, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, SY(4), 0);
    lv_obj_set_style_pad_all(s_list, SX(6), 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    rebuild_list();
    return root;
}

/* ------ 导航回调 ------ */
static void go_back_cb(lv_event_t * e)
{
    (void)e;
    /* 用户管理现为底部「用户」页签（一级页），返回主页即可 */
    ui_switch_page(PAGE_HOME);
}

/* ================================================================
 *  行渲染 — 每个用户一行，包含所有信息和操作按钮
 *  行高 SY(56)，内部 flex row 横排：
 *  [👤username] [spacer] [role-pill] [status-dot] [改密][停用][删除]
 * ================================================================ */
static void users_list_loaded(safe_user_t * us, int n)
{
    lv_obj_clean(s_list);

    /* ★ 先统计「启用中的管理员」数量：为 1 时该管理员的停用/删除必须锁死，
     * 否则设备将进入无人可管理的死锁状态（没有任何账号能再进设置页）。 */
    int admin_enabled = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].role, "admin") == 0 && us[i].enabled) admin_enabled++;
    }

    for (int i = 0; i < n; i++) {
        /* ★ 提前声明 is_admin，因为头像圆 (icon) 块在 role_pill 之前就要用 */
        bool is_admin = (strcmp(us[i].role, "admin") == 0);
        /* 唯一还活着的管理员 → 停用/删除 置灰不可点 */
        bool last_admin = (is_admin && us[i].enabled && admin_enabled <= 1);

        /* 行容器 */
        lv_obj_t * row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), SY(56));
        lv_obj_add_style(row, &st_panel2, 0);
        lv_obj_set_style_radius(row, SX(8), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, SX(8), 0);
        lv_obj_set_style_pad_row(row, 0, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);  /* 防止行内滚动 */

        /* --- 左侧：头像圆 + 用户名 --- */
        /* ★ 修复：原来的 ▸ 字符(Noto CJK 没收录)→ 改用直径 28 的小圆代替，纯色块无字形依赖；
         *      admin 用 accent 强调色，普通用户用 panel2（次面板）边框色 */
        lv_obj_t * icon = lv_obj_create(row);
        lv_obj_set_size(icon, SX(28), SX(28));
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
        char initial[4] = { us[i].name[0] ? us[i].name[0] : '?', '\0' };
        lv_obj_t * initial_lbl = lv_label_create(icon);
        lv_label_set_text(initial_lbl, initial);
        lv_obj_set_style_text_font(initial_lbl, app_font_scaled(13), 0);
        lv_obj_set_style_text_color(initial_lbl, is_admin ? theme_color(TH_ACCENT_INK) : theme_color(TH_TEXT), 0);
        lv_obj_center(initial_lbl);

        lv_obj_t * nm = lv_label_create(row);
        lv_label_set_text(nm, us[i].name);
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font_scaled(17), 0);
        lv_obj_set_style_text_font(nm, app_font_scaled(17), 0);

        /* 弹性占位，把角色推到中间偏左 */
        lv_obj_t * sp1 = lv_obj_create(row);
        lv_obj_set_flex_grow(sp1, 1);
        lv_obj_set_style_bg_opa(sp1, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp1, 0, 0);

        /* --- 角色（药丸标签） --- */
        lv_obj_t * role_pill = lv_button_create(row);
        lv_obj_set_height(role_pill, SY(26));
        lv_obj_add_style(role_pill, &st_ghost_btn, 0);
        lv_obj_set_style_radius(role_pill, SX(13), 0);
        lv_obj_set_style_pad_left(role_pill, SX(10), 0);
        lv_obj_set_style_pad_right(role_pill, SX(10), 0);
        /* is_admin 已在循环开头声明；admin 用 accent 色 */
        if (is_admin) {
            lv_obj_add_style(role_pill, &st_accent_btn, 0);
        } else {
            lv_obj_set_style_bg_color(role_pill, theme_color(TH_PANEL2), 0);
            lv_obj_set_style_bg_opa(role_pill, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(role_pill, theme_color(TH_TEXT_MUT), 0);
            lv_obj_set_style_border_width(role_pill, 1, 0);
            lv_obj_set_style_border_side(role_pill, LV_BORDER_SIDE_FULL, 0);
        }

        lv_obj_t * rl = lv_label_create(role_pill);
        lv_label_set_text(rl, is_admin ? "管理员" : "普通用户");
        lv_obj_set_style_text_font(rl, app_font_scaled(12), 0);
        if (!is_admin) lv_obj_set_style_text_color(rl, theme_color(TH_TEXT_MUT), 0);
        lv_obj_center(rl);

        /* --- 状态指示器 --- */
        lv_obj_t * st_lbl = lv_label_create(row);
        if (us[i].enabled) {
            lv_label_set_text(st_lbl, "● 启用");  /* ● 启用 */
            lv_obj_add_style(st_lbl, &st_ok_text, 0);
        } else {
            lv_label_set_text(st_lbl, "● 停用");  /* ● 停用 */
            lv_obj_add_style(st_lbl, &st_warn_text, 0);
        }
        lv_obj_set_style_text_font(st_lbl, app_font_scaled(13), 0);

        /* 小间距 */
        lv_obj_t * sp2 = lv_obj_create(row);
        lv_obj_set_size(sp2, SX(8), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp2, 0, 0);

        /* --- 右侧操作按钮组 --- */
        int uid = us[i].id;

        /* 改密 */
        lv_obj_t * b_pwd = lv_button_create(row);
        lv_obj_set_size(b_pwd, SX(58), SY(34));
        lv_obj_add_style(b_pwd, &st_ghost_btn, 0);
        lv_obj_add_event_cb(b_pwd, row_pwd_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
        lv_obj_t * bpwl = lv_label_create(b_pwd);
        lv_label_set_text(bpwl, "改密");   /* 直接写 UTF-8 源字符，不要用 hex escape */
        lv_obj_set_style_text_font(bpwl, app_font_scaled(12), 0);
        lv_obj_center(bpwl);

        /* 启用/停用 —— 末位管理员锁死 */
        lv_obj_t * b_tog = lv_button_create(row);
        lv_obj_set_size(b_tog, SX(58), SY(34));
        lv_obj_add_style(b_tog, &st_ghost_btn, 0);
        if (last_admin) {
            lv_obj_add_state(b_tog, LV_STATE_DISABLED);
            lv_obj_set_style_opa(b_tog, LV_OPA_40, LV_STATE_DISABLED);
        } else {
            lv_obj_add_event_cb(b_tog, row_tog_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
        }
        lv_obj_t * btgl = lv_label_create(b_tog);
        lv_label_set_text(btgl, us[i].enabled ? "停用" : "启用");
        lv_obj_set_style_text_font(btgl, app_font_scaled(12), 0);
        lv_obj_center(btgl);

        /* 删除 —— 末位管理员锁死 */
        lv_obj_t * b_del = lv_button_create(row);
        lv_obj_set_size(b_del, SX(58), SY(34));
        lv_obj_add_style(b_del, &st_danger_btn, 0);
        if (last_admin) {
            lv_obj_add_state(b_del, LV_STATE_DISABLED);
            lv_obj_set_style_opa(b_del, LV_OPA_40, LV_STATE_DISABLED);
        } else {
            lv_obj_add_event_cb(b_del, row_del_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
        }
        lv_obj_t * bdll = lv_label_create(b_del);
        lv_label_set_text(bdll, "删除");
        lv_obj_set_style_text_font(bdll, app_font_scaled(12), 0);
        lv_obj_center(bdll);
    }
}

static void rebuild_list(void)
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

/* ================================================================
 *  弹窗基础（保持不变）
 * ================================================================ */
static lv_obj_t * dlg_open(const char *title, int w, int h)
{
    close_dlg();
    lv_obj_t * scr = lv_screen_active();

    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_add_event_cb(s_ov, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, w, h);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    /* ★ 修复：弹窗顶部距屏顶 SY(10)，底部 ≈ 360+10=370；1.0x 板键盘从 y=380 起，
     *      留 10px 缝隙；1.8x PC 同样不挡。 */
    lv_obj_align(s_win, LV_ALIGN_TOP_MID, 0, SY(10));
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* ★ 修复：原 8 间距太小，叠加 st_panel 自带 pad_all=16，3 个 textarea+2 按钮共 5 行溢出窗口；改 6 */
    lv_obj_set_style_pad_row(s_win, SY(6), 0);

    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, title);
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);

    s_kb = lv_keyboard_create(s_ov);
    lv_obj_set_size(s_kb, lv_pct(100), SY(220));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, theme_color(TH_PANEL), 0);
    /* ★★ 关键修复（2026-08-28）：lv_keyboard 是 lv_buttonmatrix 的【子类】，它没有子对象。
     * 旧实现遍历 lv_obj_get_child(s_kb) 逐个设字体 —— child_cnt 恒为 0，整段代码是空转，
     * 键盘一直用 LV_FONT_DEFAULT(montserrat_14)，这就是"字母太小根本看不清"的真正原因。
     *
     * 正确做法：btnmatrix 的按键文本属于 LV_PART_ITEMS，直接给它设字号即可。
     * 字体必须选 Montserrat 而不是中文 fm：键盘上的控制键是 LV_SYMBOL_*(⌫ ↩ ⏎)，
     * 这些码位 Noto CJK / 嵌入中文字体里没有，套中文字体会渲染成方块。
     * 字母 / 数字本身也是 ASCII，Montserrat 完全覆盖，不存在缺字。 */
    lv_obj_set_style_text_font(s_kb, app_montserrat_scaled(KB_FONT_BASE_TEXT), LV_PART_ITEMS);

    /* 默认聚焦逻辑移到了 dlg_textarea(idx==1) 内部（dlg_open 返回时 s_dlg_tas 还是 NULL，
     * 必须等 3 个 textarea 创建完才能 set_textarea） */
    return s_win;
}

static void close_dlg(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    s_ov = NULL; s_win = NULL; s_kb = NULL; s_msg = NULL;
}

static void dlg_set_msg(const char *text)
{
    if (s_msg) lv_label_set_text(s_msg, text);
}

static lv_obj_t * dlg_textarea(const char *label, bool password, bool number, int idx)
{
    if (idx >= 0 && idx < 3) s_dlg_tas[idx] = NULL;  /* 先清空，防 dialog 复用残留 */
    lv_obj_t * ta = lv_textarea_create(s_win);
    lv_obj_set_size(ta, SX(300), SY(40));
    lv_obj_add_style(ta, &st_panel2, 0);
    lv_obj_set_style_radius(ta, SX(6), 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, label);
    /* 占位文字明确设色：避免随主题切换后看不清或与背景融为一体 */
    lv_obj_set_style_text_color(ta, theme_color(TH_TEXT_PLACEHOLDER), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(ta, app_font_scaled(14), LV_PART_TEXTAREA_PLACEHOLDER);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
        lv_textarea_set_max_length(ta, 8);
    }
    lv_obj_add_event_cb(ta, kb_focus_cb, LV_EVENT_FOCUSED, (void *)(uintptr_t)(number ? 1 : 0));
    lv_obj_set_style_text_font(ta, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(ta, theme_color(TH_TEXT), 0);
    if (idx >= 0 && idx < 3) s_dlg_tas[idx] = ta;
    /* ★★ idx==1（PIN/新 PIN）：创建后立即 bind 键盘 + 切 NUMBER 模式。
     * 关键：dlg_textarea 是在 dlg_open 返回之后才被调用，所以 dlg_open 末尾做 focus
     * 已经来不及（s_dlg_tas[1] 还是 NULL）。必须在这里做。 */
    if (idx == 1 && s_kb) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_keyboard_set_mode(s_kb, number ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_set_style_text_font(s_kb,
            app_montserrat_scaled(number ? KB_FONT_BASE_NUMBER : KB_FONT_BASE_TEXT), LV_PART_ITEMS);
    }
    return ta;
}

static void kb_focus_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target(e);
    bool number = (bool)(uintptr_t)lv_event_get_user_data(e);
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_keyboard_set_mode(s_kb, number ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        /* 数字模式键更少更大 → 再放大一档 */
        lv_obj_set_style_text_font(s_kb,
            app_montserrat_scaled(number ? KB_FONT_BASE_NUMBER : KB_FONT_BASE_TEXT), LV_PART_ITEMS);
    }
}

static void dlg_cancel_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

static void dlg_ok_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

/* ================================================================
 *  添加用户（保持不变）
 * ================================================================ */
static void add_btn_cb(lv_event_t * e)
{
    (void)e;
    dlg_add_user();
}

static void dlg_add_user(void)
{
    lv_obj_t * win = dlg_open("添加用户", SX(380), SY(360));  /* 添加用户 */
    (void)win;
    dlg_textarea("用户名(字母数字)", false, false, 0);
    dlg_textarea("PIN(4-8 位数字)",   true,  true,  1);  /* 默认焦点 */
    dlg_textarea("确认 PIN",          true,  true,  2);

    lv_obj_t * save = lv_button_create(s_win);
    lv_obj_set_size(save, SX(160), SY(44));
    lv_obj_add_style(save, &st_accent_btn, 0);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");  /* 保存 */
    lv_obj_set_style_text_font(sl, app_font_scaled(20), 0);
    lv_obj_center(sl);

    /* 取消按钮 — ★ 不用 st_ghost_btn（实测该样式的 text_color 有时不级联到 label，
     *  "取消"会显示成深色方块），改为手动设置所有样式 */
    lv_obj_t * canc = lv_button_create(s_win);
    lv_obj_set_size(canc, SX(140), SY(44));
    lv_obj_set_style_bg_color(canc, theme_color(TH_PANEL), 0);
    lv_obj_set_style_bg_opa(canc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(canc, theme_color(TH_BORDER), 0);
    lv_obj_set_style_border_width(canc, 1, 0);
    lv_obj_set_style_radius(canc, SX(10), 0);
    /* 文字颜色必须跟随主题，浅色主题下白色会融入面板背景 */
    lv_obj_set_style_text_color(canc, theme_color(TH_TEXT), 0);
    lv_obj_add_event_cb(canc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cnl = lv_label_create(canc);
    lv_obj_set_style_text_font(cnl, app_font_scaled(16), 0);
    lv_obj_set_style_text_color(cnl, theme_color(TH_TEXT), 0);
    lv_label_set_text(cnl, "取消");
    lv_obj_center(cnl);
}

static int dlg_get_tas(lv_obj_t * tas[3], int max)
{
    int ti = 0;
    uint32_t cnt = lv_obj_get_child_count(s_win);
    for (uint32_t i = 0; i < cnt && ti < max; i++) {
        lv_obj_t * c = lv_obj_get_child(s_win, i);
        if (lv_obj_check_type(c, &lv_textarea_class)) tas[ti++] = c;
    }
    return ti;
}

static void add_user_worker(void * p)
{
    add_user_job_t * a = (add_user_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    int maxid = 0;
    for (int i = 0; i < n; i++) {
        if (us[i].id > maxid) maxid = us[i].id;
        if (strcmp(us[i].name, a->name) == 0) { user_list_free(us); a->result = -2; return; }
    }
    user_list_free(us);

    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = maxid + 1;
    strncpy(u.name, a->name, sizeof(u.name) - 1);
    strncpy(u.role, "user", sizeof(u.role) - 1);
    strncpy(u.auth_method, "pin", sizeof(u.auth_method) - 1);
    u.enabled = true;
    uint8_t salt[16];
    pin_hash(a->pin, salt, u.pin_hash);
    salt_to_hex(salt, u.pin_salt);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(u.created_at, sizeof(u.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);

    a->result = user_add(&u);
    if (a->result == 0) log_append("user_add", "admin", 1, a->name);
}

static void add_user_done(void * p)
{
    add_user_job_t * a = (add_user_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        rebuild_list();
    } else if (a->result == -2) {
        dlg_set_msg("用户名已存在");  /* 用户名已存在 */
    } else {
        dlg_set_msg("保存失败");  /* 保存失败 */
    }
    free(a);
}

static void save_add_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * tas[3];
    int ti = dlg_get_tas(tas, 3);
    if (ti < 3) return;

    const char * name = lv_textarea_get_text(tas[0]);
    const char * pin1 = lv_textarea_get_text(tas[1]);
    const char * pin2 = lv_textarea_get_text(tas[2]);

    const safe_policy_t * pol = user_policy();
    if (!name[0]) { dlg_set_msg("用户名不能为空"); return; }  /* 用户名不能为空 */
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) {
            dlg_set_msg("用户名仅限字母数字");  /* 用户名仅限字母数字 */
            return;
        }
    }
    size_t plen = strlen(pin1);
    if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
        dlg_set_msg("PIN 长度需 4-8 位");  /* PIN 长度需 4-8 位 */
        return;
    }
    if (strcmp(pin1, pin2) != 0) {
        dlg_set_msg("两次 PIN 不一致");  /* 两次 PIN 不一致 */
        return;
    }

    add_user_job_t * a = (add_user_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->pin, pin1, sizeof(a->pin) - 1);
    worker_post(add_user_worker, a, add_user_done);
}

/* ================================================================
 *  修改密码（保持不变）
 * ================================================================ */
static void pwd_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    dlg_change_pwd();
}

static void dlg_change_pwd(void)
{
    /* 改的是管理员吗？是则保存前强制走动态码二次确认 */
    s_pwd_is_admin = user_is_admin(s_sel_id);
    lv_obj_t * win = dlg_open("修改密码", SX(380), SY(420));  /* 修改密码 */
    (void)win;
    dlg_textarea("旧 PIN",         true, true, 0);
    dlg_textarea("新 PIN(4-8 位)",  true, true, 1);  /* 默认焦点 */
    dlg_textarea("确认新 PIN",       true, true, 2);

    lv_obj_t * save = lv_button_create(s_win);
    lv_obj_set_size(save, SX(160), SY(44));
    lv_obj_add_style(save, &st_accent_btn, 0);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, save_pwd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");  /* 保存 */
    lv_obj_set_style_text_font(sl, app_font_scaled(16), 0);
    lv_obj_center(sl);
}

static void chg_pwd_worker(void * p)
{
    chg_pwd_job_t * a = (chg_pwd_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    safe_user_t u;
    bool found = false;
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { u = us[i]; found = true; break; }
    user_list_free(us);
    if (!found) { a->result = -1; return; }

    if (user_verify_pin(u.name, a->oldpin) != 0) { a->result = 1; return; }

    uint8_t salt[16];
    pin_hash(a->newpin, salt, u.pin_hash);
    salt_to_hex(salt, u.pin_salt);
    u.failed_attempts = 0;
    u.lock_until = 0;
    a->result = (user_update(&u) == 0) ? 0 : -1;
    if (a->result == 0) log_append("pwd_change", u.name, 1, "pin updated");
}

static void chg_pwd_done(void * p)
{
    chg_pwd_job_t * a = (chg_pwd_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        rebuild_list();
    } else if (a->result == 1) {
        dlg_set_msg("旧 PIN 错误");  /* 旧 PIN 错误 */
    } else {
        dlg_set_msg("保存失败");  /* 保存失败 */
    }
    free(a);
}

static void save_pwd_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * tas[3];
    int ti = dlg_get_tas(tas, 3);
    if (ti < 3) return;

    const char * oldp = lv_textarea_get_text(tas[0]);
    const char * newp = lv_textarea_get_text(tas[1]);
    const char * newp2 = lv_textarea_get_text(tas[2]);

    const safe_policy_t * pol = user_policy();
    size_t plen = strlen(newp);
    if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
        dlg_set_msg("新 PIN 长度需 4-8 位");  /* 新 PIN 长度需 4-8 位 */
        return;
    }
    if (strcmp(newp, newp2) != 0) {
        dlg_set_msg("两次新 PIN 不一致");  /* 两次新 PIN 不一致 */
        return;
    }

    /* ★ 敏感操作（管理员改密）：先过动态码二次确认，通过后才提交改密 */
    if (s_pwd_is_admin) {
        strncpy(s_pending_old, oldp, sizeof(s_pending_old) - 1);
        strncpy(s_pending_new, newp, sizeof(s_pending_new) - 1);
        close_dlg();          /* 收起 PIN 弹窗 */
        pwd_otp_show();       /* 弹动态码键盘 */
        return;
    }
    post_chg_pwd(s_sel_id, oldp, newp);
}

/* 提交改密（管理员已通过动态码，或非管理员直接走这里） */
static void post_chg_pwd(int id, const char *oldp, const char *newp)
{
    chg_pwd_job_t * a = (chg_pwd_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = id;
    strncpy(a->oldpin, oldp, sizeof(a->oldpin) - 1);
    strncpy(a->newpin, newp, sizeof(a->newpin) - 1);
    worker_post(chg_pwd_worker, a, chg_pwd_done);
}

/* 目标用户是否为管理员（同步读 users.json，文件小、无阻塞感） */
static bool user_is_admin(int id)
{
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); return false; }
    bool r = false;
    for (int i = 0; i < n; i++) {
        if (us[i].id == id) { r = (strcmp(us[i].role, "admin") == 0); break; }
    }
    user_list_free(us);
    return r;
}

/* ================================================================
 *  启用/停用（保持不变）
 * ================================================================ */
static void tog_worker(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    safe_user_t u;
    bool found = false;
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { u = us[i]; found = true; break; }
    user_list_free(us);
    if (!found) { a->result = -1; return; }

    u.enabled = !u.enabled;
    a->result = (user_update(&u) == 0) ? 0 : -1;
    if (a->result == 0) log_append("user_modify", u.name, 1, u.enabled ? "enabled" : "disabled");
}

static void tog_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        rebuild_list();
    }
    free(a);
}

static void toggle_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    user_op_job_t * a = (user_op_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = s_sel_id;
    worker_post(tog_worker, a, tog_done);
}

/* ================================================================
 *  管理员二次校验弹窗（删除用户前置）
 *  —— 自绘 3×4 数字小键盘，不复用 lv_keyboard：
 *     1) 与表单弹窗的 s_ov / s_kb 完全解耦，不会互相踩踏；
 *     2) 纯数字输入场景，键更大、误触更少（触屏优先）。
 * ================================================================ */
static void auth_update_disp(void)
{
    if (!s_auth_disp) return;
    size_t len = strlen(s_auth_pin);
    char masked[16];
    for (size_t i = 0; i < len; i++) masked[i] = '*';
    masked[len] = '\0';
    lv_label_set_text(s_auth_disp, len ? masked : "——");
}

static void auth_close(void)
{
    if (s_auth_ov) lv_obj_delete(s_auth_ov);
    s_auth_ov = NULL; s_auth_disp = NULL; s_auth_msg = NULL;
    s_auth_pin[0] = '\0';
}

static void auth_key_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    static const char * KEYS[12] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "退格", "0", "清空",
    };
    const char * key = KEYS[idx];
    if (strcmp(key, "清空") == 0) {
        s_auth_pin[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_auth_pin);
        if (len > 0) s_auth_pin[len - 1] = '\0';
    } else {
        size_t len = strlen(s_auth_pin);
        if (len < 8) { s_auth_pin[len] = key[0]; s_auth_pin[len + 1] = '\0'; }
    }
    auth_update_disp();
}

static void auth_cancel_cb(lv_event_t * e)
{
    (void)e;
    auth_close();
}

/* PBKDF2 校验在后台线程跑，结果回主线程 */
static void auth_result(int r)
{
    if (r == 0) {
        auth_close();
        astore_load_users(del_check_loaded);   /* 校验通过 → 进入删除确认 */
    } else {
        s_auth_pin[0] = '\0';
        auth_update_disp();
        if (s_auth_msg) {
            lv_label_set_text(s_auth_msg, (r == 2) ? "管理员已锁定，请稍后再试" : "PIN 错误，请重试");
        }
    }
}

static void auth_ok_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_auth_pin) < 4) {
        if (s_auth_msg) lv_label_set_text(s_auth_msg, "PIN 至少 4 位");
        return;
    }
    astore_verify_admin(s_auth_pin, auth_result);
}

static void auth_show(void)
{
    auth_close();
    lv_obj_t * scr = lv_screen_active();

    s_auth_ov = lv_obj_create(scr);
    lv_obj_set_size(s_auth_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_auth_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_auth_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_auth_ov, 0, 0);
    lv_obj_add_event_cb(s_auth_ov, auth_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * win = lv_obj_create(s_auth_ov);
    lv_obj_set_size(win, SX(380), SY(430));
    lv_obj_add_style(win, &st_panel, 0);
    lv_obj_set_style_radius(win, SX(16), 0);
    lv_obj_set_style_pad_all(win, 0, 0);       /* 下面全用绝对坐标，去掉默认 padding */
    lv_obj_center(win);

    lv_obj_t * t = lv_label_create(win);
    lv_label_set_text(t, "管理员验证");
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    lv_obj_set_width(t, SX(380));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, SY(14));

    lv_obj_t * sub = lv_label_create(win);
    lv_label_set_text(sub, "删除用户需二次验证管理员 PIN");
    lv_obj_set_style_text_font(sub, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(sub, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_width(sub, SX(380));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sub, 0, SY(44));

    s_auth_disp = lv_label_create(win);
    lv_label_set_text(s_auth_disp, "——");
    lv_obj_add_style(s_auth_disp, &st_text, 0);
    lv_obj_set_style_text_font(s_auth_disp, app_font_scaled(28), 0);
    lv_obj_set_style_pad_all(s_auth_disp, 0, 0);
    lv_obj_set_style_text_align(s_auth_disp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_auth_disp, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_auth_disp, SX(8), 0);
    lv_obj_set_size(s_auth_disp, SX(240), SY(50));
    lv_obj_set_pos(s_auth_disp, SX(70), SY(70));

    s_auth_msg = lv_label_create(win);
    lv_label_set_text(s_auth_msg, "请输入管理员 PIN");
    lv_obj_set_style_text_font(s_auth_msg, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(s_auth_msg, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_width(s_auth_msg, SX(380));
    lv_obj_set_style_text_align(s_auth_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_auth_msg, 0, SY(128));

    /* 3×4 数字键盘：键宽 100、列距 120、行距 55 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    const int32_t ROW_Y[4] = { 160, 215, 270, 325 };
    const int32_t KEY_X[3] = { 20, 140, 260 };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(win);
            lv_obj_set_size(k, SX(100), SY(46));
            lv_obj_set_pos(k, SX(KEY_X[c]), SY(ROW_Y[r]));
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, SX(8), 0);
            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(22), 0);
            lv_obj_center(kl);
            lv_obj_add_event_cb(k, auth_key_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)(r * 3 + c));
        }
    }

    lv_obj_t * canc = lv_button_create(win);
    lv_obj_set_size(canc, SX(160), SY(44));
    lv_obj_set_pos(canc, SX(20), SY(376));
    lv_obj_add_style(canc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(canc, auth_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(canc);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, app_font_scaled(16), 0);
    lv_obj_center(cl);

    lv_obj_t * ok = lv_button_create(win);
    lv_obj_set_size(ok, SX(160), SY(44));
    lv_obj_set_pos(ok, SX(200), SY(376));
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, auth_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ol = lv_label_create(ok);
    lv_label_set_text(ol, "确认");
    lv_obj_set_style_text_font(ol, app_font_scaled(16), 0);
    lv_obj_center(ol);
}

/* ================================================================
 *  删除（保持不变）
 * ================================================================ */
static void del_check_loaded(safe_user_t * us, int n)
{
    safe_user_t u;
    bool found = false;
    int admin_enabled = 0;
    for (int i = 0; i < n; i++) {
        /* 只统计「启用中」的管理员：停用状态的管理员救不了死锁 */
        if (strcmp(us[i].role, "admin") == 0 && us[i].enabled) admin_enabled++;
        if (us[i].id == s_sel_id) { u = us[i]; found = true; }
    }
    if (!found) return;

    /* 纵深防御：列表层已把按钮置灰，这里再拦一次（防异步竞态 / 其他入口） */
    if (strcmp(u.role, "admin") == 0 && admin_enabled <= 1) {
        dlg_tip("至少保留一名管理员");
        return;
    }
    char msg[96];
    /* ★ 直接写 UTF-8 源字符：连续 hex escape("\xE7\xA1\xAE...") 会被 GCC 贪婪吞并
     * 成单个无效码位，中文渲染成方块 —— 这是本项目踩过的坑，别再写回去了 */
    snprintf(msg, sizeof(msg), "确定删除用户 %s ？", u.name);
    dlg_confirm_del(msg);
}

static void del_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    /* ★ 敏感操作二次校验：先过管理员 PIN，通过后才走删除确认 */
    auth_show();
}

static void dlg_confirm_del(const char *msg)
{
    dlg_open("删除确认", 360, 180);  /* 删除确认 */
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, msg);
    lv_obj_add_style(m, &st_text, 0);
    lv_obj_set_style_text_font(m, app_font_scaled(16), 0);

    const int32_t btn_y = lv_obj_get_height(s_win) - 16 - 44 - 16;
    const int32_t btn_x = (lv_obj_get_width(s_win) - 32 - 280) / 2 + 16;

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 136, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_add_flag(cc, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font_scaled(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * yy = lv_button_create(s_win);
    lv_obj_set_size(yy, 136, 44);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
    lv_obj_add_flag(yy, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_style(yy, &st_danger_btn, 0);
    lv_obj_add_event_cb(yy, do_del_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yyl = lv_label_create(yy);
    lv_label_set_text(yyl, "删除");  /* 删除 */
    lv_obj_set_style_text_font(yyl, app_font_scaled(14), 0);
    lv_obj_center(yyl);
}

static void dlg_tip(const char *text)
{
    dlg_open("提示", 340, 150);  /* 提示 */
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, text);
    lv_obj_add_style(m, &st_warn_text, 0);
    lv_obj_set_style_text_font(m, app_font_scaled(16), 0);

    lv_obj_t * okb = lv_button_create(s_win);
    lv_obj_set_size(okb, 120, 40);
    lv_obj_add_style(okb, &st_ghost_btn, 0);
    lv_obj_add_event_cb(okb, dlg_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ok = lv_label_create(okb);
    lv_label_set_text(ok, "知道了");  /* 知道了 */
    lv_obj_set_style_text_font(ok, app_font_scaled(14), 0);
    lv_obj_center(ok);
}

static void del_worker(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    char name[32] = {0};
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0) { user_list_free(us); a->result = -1; return; }
    for (int i = 0; i < n; i++) if (us[i].id == a->id) { strncpy(name, us[i].name, sizeof(name) - 1); break; }
    user_list_free(us);

    a->result = user_del(a->id);
    if (a->result == 0) log_append("user_del", "admin", 1, name);
}

static void del_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        close_dlg();
        s_sel_id = -1;
        rebuild_list();
    }
    free(a);
}

static void do_del_cb(lv_event_t * e)
{
    (void)e;
    user_op_job_t * a = (user_op_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->id = s_sel_id;
    worker_post(del_worker, a, del_done);
}











/* ================================================================
 *  修改管理员密码 — 动态码二次确认（FR-3 敏感操作）
 *  —— 自绘 6 位动态码键盘，复用 auth_show 的视觉范式；
 *     校验走 backend_admin_verify_totp（任一启用且开 TOTP 的管理员）。
 * ================================================================ */
static void pwd_otp_update_disp(void)
{
    if (!s_otp_disp) return;
    size_t len = strlen(s_otp_pin);
    char masked[8];
    for (size_t i = 0; i < len; i++) masked[i] = s_otp_pin[i];
    masked[len] = '\0';
    lv_label_set_text(s_otp_disp, len ? masked : "------");
}

static void pwd_otp_close(void)
{
    if (s_otp_ov) lv_obj_delete(s_otp_ov);
    s_otp_ov = NULL; s_otp_disp = NULL; s_otp_msg = NULL;
    s_otp_pin[0] = '\0';
}

static void pwd_otp_key_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    static const char * KEYS[12] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "退格", "0", "清空",
    };
    const char * key = KEYS[idx];
    if (strcmp(key, "清空") == 0) {
        s_otp_pin[0] = '\0';
    } else if (strcmp(key, "退格") == 0) {
        size_t len = strlen(s_otp_pin);
        if (len > 0) s_otp_pin[len - 1] = '\0';
    } else {
        size_t len = strlen(s_otp_pin);
        if (len < 6) { s_otp_pin[len] = key[0]; s_otp_pin[len + 1] = '\0'; }
    }
    pwd_otp_update_disp();
}

static void pwd_otp_cancel_cb(lv_event_t * e)
{
    (void)e;
    pwd_otp_close();
}

static void pwd_otp_ok_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_otp_pin) != 6) {
        if (s_otp_msg) lv_label_set_text(s_otp_msg, "动态码为 6 位");
        return;
    }
    auth_result_t r = backend_admin_verify_totp(s_otp_pin);
    if (r == AUTH_OK) {
        pwd_otp_close();
        post_chg_pwd(s_sel_id, s_pending_old, s_pending_new);
    } else {
        s_otp_pin[0] = '\0';
        pwd_otp_update_disp();
        if (s_otp_msg) {
            lv_label_set_text(s_otp_msg,
                (r == AUTH_REPLAY) ? "该动态码已使用，请等下一周期" : "动态码错误，请重试");
        }
    }
}

static void pwd_otp_show(void)
{
    pwd_otp_close();
    lv_obj_t * scr = lv_screen_active();

    s_otp_ov = lv_obj_create(scr);
    lv_obj_set_size(s_otp_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_otp_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_otp_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_otp_ov, 0, 0);
    lv_obj_add_event_cb(s_otp_ov, pwd_otp_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * win = lv_obj_create(s_otp_ov);
    lv_obj_set_size(win, SX(380), SY(430));
    lv_obj_add_style(win, &st_panel, 0);
    lv_obj_set_style_radius(win, SX(16), 0);
    lv_obj_set_style_pad_all(win, 0, 0);       /* 下面全用绝对坐标，去掉默认 padding */
    lv_obj_center(win);

    lv_obj_t * t = lv_label_create(win);
    lv_label_set_text(t, "管理员验证");
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    lv_obj_set_width(t, SX(380));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, SY(14));

    lv_obj_t * sub = lv_label_create(win);
    lv_label_set_text(sub, "修改管理员密码需验证动态码");
    lv_obj_set_style_text_font(sub, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(sub, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_width(sub, SX(380));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sub, 0, SY(44));

    s_otp_disp = lv_label_create(win);
    lv_label_set_text(s_otp_disp, "------");
    lv_obj_add_style(s_otp_disp, &st_text, 0);
    lv_obj_set_style_text_font(s_otp_disp, app_font_scaled(28), 0);
    lv_obj_set_style_text_align(s_otp_disp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_otp_disp, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_radius(s_otp_disp, SX(8), 0);
    lv_obj_set_size(s_otp_disp, SX(240), SY(50));
    lv_obj_set_pos(s_otp_disp, SX(70), SY(70));

    s_otp_msg = lv_label_create(win);
    lv_label_set_text(s_otp_msg, "请输入 6 位动态码");
    lv_obj_set_style_text_font(s_otp_msg, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(s_otp_msg, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_width(s_otp_msg, SX(380));
    lv_obj_set_style_text_align(s_otp_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_otp_msg, 0, SY(128));

    /* 3×4 数字键盘：键宽 100、列距 120、行距 55 */
    static const char * KEYS[4][3] = {
        { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "退格", "0", "清空" },
    };
    const int32_t ROW_Y[4] = { 160, 215, 270, 325 };
    const int32_t KEY_X[3] = { 20, 140, 260 };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            lv_obj_t * k = lv_button_create(win);
            lv_obj_set_size(k, SX(100), SY(46));
            lv_obj_set_pos(k, SX(KEY_X[c]), SY(ROW_Y[r]));
            lv_obj_add_style(k, &st_panel2, 0);
            lv_obj_set_style_radius(k, SX(8), 0);
            lv_obj_t * kl = lv_label_create(k);
            lv_label_set_text(kl, KEYS[r][c]);
            lv_obj_add_style(kl, &st_text, 0);
            lv_obj_set_style_text_font(kl, app_font_scaled(22), 0);
            lv_obj_center(kl);
            lv_obj_add_event_cb(k, pwd_otp_key_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)(r * 3 + c));
        }
    }

    lv_obj_t * canc = lv_button_create(win);
    lv_obj_set_size(canc, SX(160), SY(44));
    lv_obj_set_pos(canc, SX(20), SY(376));
    lv_obj_add_style(canc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(canc, pwd_otp_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(canc);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, app_font_scaled(16), 0);
    lv_obj_center(cl);

    lv_obj_t * ok = lv_button_create(win);
    lv_obj_set_size(ok, SX(160), SY(44));
    lv_obj_set_pos(ok, SX(200), SY(376));
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, pwd_otp_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ol = lv_label_create(ok);
    lv_label_set_text(ol, "验证");
    lv_obj_set_style_text_font(ol, app_font_scaled(16), 0);
    lv_obj_center(ol);
}

/* ★ 测试钩子：让 main.c 的 SAFE_TEST_DLG=add_user 能直接打开"添加用户"弹窗（无需点击） */
void page_users_test_open_add_dlg(void)
{
    dlg_add_user();
}

/* ★ 测试钩子：SAFE_TEST_DLG=auth 直接打开管理员二次校验弹窗 */
void page_users_test_open_auth_dlg(void)
{
    s_sel_id = 0;
    auth_show();
}

/* ★ 测试钩子：SAFE_TEST_DLG=change_pwd 直接打开"修改密码"弹窗 */
void page_users_test_open_change_pwd_dlg(void)
{
    s_sel_id = 0;
    dlg_change_pwd();
}

/* ★ 测试钩子：SAFE_TEST_DLG=otp 直接打开"管理员动态码"弹窗（验证 FR-3 敏感操作） */
void page_users_test_open_otp_dlg(void)
{
    s_sel_id = 0;
    s_pwd_is_admin = true;
    pwd_otp_show();
}


















