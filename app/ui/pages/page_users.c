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

/* 列表容器 */
/* S1：『添加用户』按钮句柄 —— 数据读取失败时禁用它，避免用户在文件损坏的
 * 情况下点下去，把损坏的数据文件覆盖掉（不可逆）。 */
static lv_obj_t * s_add_btn = NULL;
static lv_obj_t * s_list = NULL;
/* 顶部统计行（FR-28 分区后承载「总人数 / 各角色人数」，空分区隐藏时信息不丢） */
static lv_obj_t * s_summary = NULL;

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
 *   idx 2: 确认 PIN
 *   idx 3: 有效期（天）— 仅「添加用户」弹窗且角色=临时用户
 *   idx 4: 次数上限     — 同上
 * 改密弹窗只有前 3 个；添加用户弹窗 5 个全有。 */
#define DLG_TA_MAX 5
static lv_obj_t * s_dlg_tas[DLG_TA_MAX] = {NULL, NULL, NULL, NULL, NULL};

/* 添加用户弹窗的「角色」分段选择（FR-1 角色表 / FR-9 临时授权创建入口）。
 * 顺序与 store 的合法角色一致：admin / user / temp。 */
typedef enum {
    ROLE_SEL_ADMIN = 0,
    ROLE_SEL_USER  = 1,
    ROLE_SEL_TEMP  = 2,
    ROLE_SEL_CNT   = 3
} role_sel_t;

static const char * ROLE_KEY[ROLE_SEL_CNT]  = { "admin", "user", "temp" };
static const char * ROLE_TEXT[ROLE_SEL_CNT] = { "管理员", "普通用户", "临时用户" };

static role_sel_t s_role_sel  = ROLE_SEL_USER;
static lv_obj_t * s_role_btns[ROLE_SEL_CNT] = {NULL, NULL, NULL};
static lv_obj_t * s_temp_row  = NULL;   /* 「有效期 + 次数上限」行：仅临时用户可见 */

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

/* ---- 人脸录入/删除入口（UI 现代化 ui3）----
 * 敏感操作走与「删除用户」同一套 admin PIN 二次确认，因此需要记录
 * 「本次二次确认是为了哪个人脸操作」，auth_result 通过后再分叉。
 * 录入/删除本身是异步的（face_service_*_async 立即返回），结果经
 * EV_FACE_EVENT(ENROLL_DONE/DELETE_DONE) 回来：横幅由 ui_feedback 出，
 * face_id 写回 users.json 与列表刷新由本页负责（只有本页知道操作目标）。 */
typedef enum {
    FACE_OP_NONE = 0,     /* 当前二次确认不是人脸操作（删除用户走原路径） */
    FACE_OP_ENROLL,       /* 录入人脸 */
    FACE_OP_DELETE,       /* 删除人脸 */
} face_op_t;

static face_op_t s_face_op          = FACE_OP_NONE;
static int       s_face_pending_uid = -1;   /* 应答到达后要写回 face_id 的用户 */
static int       s_face_pending_tpl = -1;   /* 删除路径：要解绑的模板号 */

/* 最近一次发起录入的用户 —— 录入失败后页面停在 PAGE_ENROLL，用户点「重新录入」
 * 时需要它重新装填 s_face_pending_uid（应答到达时 on_face_event_ui 靠它写回
 * face_id）。录入失败路径会把 s_face_pending_uid 复位成 -1，故单独留一份。 */
static int       s_last_enroll_uid  = -1;

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
static void row_face_cb(lv_event_t * e);
static void pwd_btn_cb(lv_event_t * e);
static void del_btn_cb(lv_event_t * e);
static void toggle_btn_cb(lv_event_t * e);
static void face_btn_cb(lv_event_t * e);
static void face_start_cb(lv_event_t * e);
static void face_set_worker(void * p);
static void face_set_done(void * p);
static void on_face_event_ui(ev_topic_t topic, const void * payload, void * user);
static void on_user_changed(ev_topic_t topic, const void * payload, void * user);

static void dlg_add_user(void);
static void dlg_change_pwd(void);
static void dlg_confirm_del(const char *name);
static void dlg_confirm_face(const char * msg, const char * ok_text);
static void dlg_tip(const char *text);
static void close_dlg(void);
static lv_obj_t * dlg_open(const char *title, int w, int h);
static lv_obj_t * dlg_textarea(const char *label, bool password, bool number, int idx);
static lv_obj_t * dlg_textarea_ex(lv_obj_t * parent, const char *label, bool password,
                                  bool number, int idx, int32_t w);
static void dlg_role_row(void);
static void dlg_temp_row(void);
static void dlg_btn_row(void);
static void dlg_sync_role(void);
static void role_btn_cb(lv_event_t * e);
static void kb_focus_cb(lv_event_t * e);
static void save_add_cb(lv_event_t * e);
static void save_pwd_cb(lv_event_t * e);
static void do_del_cb(lv_event_t * e);
static void dlg_cancel_cb(lv_event_t * e);
static void dlg_ok_cb(lv_event_t * e);
static void dlg_set_msg(const char *text);

/* 敏感操作二次校验（管理员 PIN） */
static void auth_show(void);
static void auth_show_ctx(const char * sub_text);
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
typedef struct {
    char    name[32];
    char    pin[16];
    char    role[16];       /* "admin" / "user" / "temp"（FR-9 创建入口） */
    int64_t valid_until;    /* 临时用户有效期截止（Unix 秒，0 = 不限） */
    int     use_limit;      /* 临时用户开锁次数上限（0 = 不限） */
    int     result;
} add_user_job_t;
typedef struct { int id; char oldpin[16]; char newpin[16]; int result; } chg_pwd_job_t;
/* del_face_id：仅「删除用户」路径使用 —— 待联动删除的模组侧模板号（-1 = 无需联动）。
 * 放在这里是因为 user_del 之后本地记录就没了，模板号必须在删之前取好。 */
typedef struct { int id; int result; int del_face_id; } user_op_job_t;

static void users_list_loaded(safe_user_t * list, int count);
static void add_group_header(lv_obj_t * parent, const char * title, int count);
static void add_user_row(const safe_user_t * u, bool is_admin, bool last_admin);
static void fmt_temp_sub(const safe_user_t * u, char * out, size_t cap);
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

/* 人脸后端是否支持录入/删除（SAFE_FACE_BACKEND=none 时按钮置灰，spec §4） */
static bool face_cap_ok(bool need_enroll)
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
lv_obj_t * page_users_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    /* 页面根不自滚动：纵向滚动统一交给列表容器 s_list（见其 scroll_dir=VER），
     * 否则整页会被拖走、顶栏跟着跑（本页 scrollbar 家族问题的同一根因）。 */
    lv_obj_set_scrollable(root, false);
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
    /* ★ LVGL v9 的 lv_obj 默认可滚动（lv_button 会在构造里自行关掉，lv_obj 不会）：
     *    所有纯装饰/布局容器一律显式关滚动，否则会出现可拖动并渲染滚动条
     *    （本页头像圆就是这个原因，见 users_list_loaded 的头像圆处注释）。 */
    lv_obj_set_scrollable(head, false);

    /* 按钮挂载到 head 后由父对象持有，无需保存句柄（原写法留下未使用变量 back，D11） */
    ui_icon_text_button(head, LV_SYMBOL_LEFT, "返回",
                        SX(78), SY(38), &st_ghost_btn,
                        theme_color(TH_TEXT), go_back_cb, NULL);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "用户管理");  /* 用户管理 */
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(24), 0);
    lv_obj_set_style_pad_left(title, SX(16), 0);

    lv_obj_t * spacer_h = lv_obj_create(head);
    lv_obj_set_flex_grow(spacer_h, 1);
    lv_obj_set_style_bg_opa(spacer_h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_h, 0, 0);
    lv_obj_set_scrollable(spacer_h, false);

    lv_obj_t * add = s_add_btn = ui_icon_text_button(head, LV_SYMBOL_PLUS, "添加用户",
                                          SX(120), SY(38), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), add_btn_cb, NULL);
    lv_obj_add_style(add, &st_accent_btn_pr, LV_STATE_PRESSED);

    /* ------ 元信息行：安全规则（左） + 用户统计（右） ------
     * 左：说明「为什么那一行按钮是灰的」，避免用户以为界面卡死。
     * 不用 st_text_mut（历史坑：该样式的 text_color 有时不级联到 label）→ 显式设色。
     * 右：FR-28 分区后空分区隐藏，人数构成改由这里承载，信息不丢。 */
    lv_obj_t * meta = lv_obj_create(root);
    lv_obj_set_size(meta, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta, 0, 0);
    lv_obj_set_style_pad_all(meta, 0, 0);
    lv_obj_set_scrollable(meta, false);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * rule = lv_label_create(meta);
    lv_label_set_text(rule, "安全规则：系统必须保留至少 1 名「启用」状态的管理员");
    lv_obj_set_style_text_font(rule, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(rule, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_flex_grow(rule, 1);

    s_summary = lv_label_create(meta);
    lv_label_set_text(s_summary, "共 0 位用户");
    lv_obj_set_style_text_font(s_summary, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(s_summary, theme_color(TH_TEXT_MUT), 0);

    /* ------ 表头 ------
     * FR-28 落地后「角色」列已由分组标题承载，行内再挂一个角色药丸就是重复信息
     * —— 故表头与行内都去掉角色列，腾出的横向空间留给操作按钮（图标 + 文案）。 */
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
    /* 与行内左右内缩对齐：列表容器 s_list pad 6 + 行内 pad 10 = 16 */
    lv_obj_set_style_pad_left(hdr, SX(16), 0);
    lv_obj_set_style_pad_right(hdr, SX(16), 0);
    lv_obj_set_scrollable(hdr, false);

    /* 表头列 — 与行内**逐段等宽对齐**（行结构：头像28 + 8 + 用户名170 + 8 + 弹性
     * + 8 + 状态64 + 8 + 8 + 8 + 操作组334）。表头同构排布：用户名 206、弹性占位、
     * 状态 64、8、操作 334 —— 两行的固定宽度之和相等（644），弹性占位吸收的宽差
     * 一致，所以「状态」「操作」正好压在行内对应列上方。 */
    lv_obj_t * h_name = lv_label_create(hdr);
    lv_label_set_text(h_name, "用户名");  /* 用户名 */
    lv_obj_set_style_text_font(h_name, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_name, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_name, SX(206), LV_SIZE_CONTENT);

    lv_obj_t * h_sp = lv_obj_create(hdr);
    lv_obj_set_flex_grow(h_sp, 1);
    lv_obj_set_style_bg_opa(h_sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_sp, 0, 0);
    lv_obj_set_scrollable(h_sp, false);

    lv_obj_t * h_st = lv_label_create(hdr);
    lv_label_set_text(h_st, "状态");  /* 状态 */
    lv_obj_set_style_text_font(h_st, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_st, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_st, SX(64), LV_SIZE_CONTENT);

    lv_obj_t * h_sp2 = lv_obj_create(hdr);
    lv_obj_set_size(h_sp2, SX(8), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(h_sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_sp2, 0, 0);
    lv_obj_set_scrollable(h_sp2, false);

    lv_obj_t * h_ops = lv_label_create(hdr);
    lv_label_set_text(h_ops, "操作");  /* 操作 */
    lv_obj_set_style_text_font(h_ops, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_ops, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_ops, SX(334), LV_SIZE_CONTENT);

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

    /* 人脸录入/删除的异步应答（UI 现代化 ui3）：本页订阅 ENROLL_DONE / DELETE_DONE，
     * 负责把模组分配的模板号写回 users.json 并刷新列表；横幅由 ui_feedback 出。 */
    event_bus_subscribe(EV_FACE_EVENT, on_face_event_ui, NULL);
    /* 别处改动了用户数据 → 重新拉列表（见 on_user_changed 的注释）。 */
    event_bus_subscribe(EV_USER_CHANGED, on_user_changed, NULL);
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
    lv_obj_set_style_pad_top(h, SY(9), 0);
    lv_obj_set_style_pad_bottom(h, SY(6), 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(h, false);

    lv_obj_t * tl = lv_label_create(h);
    lv_label_set_text(tl, title);
    lv_obj_add_style(tl, &st_text, 0);
    lv_obj_set_style_text_font(tl, app_font_scaled(13), 0);

    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d 人", count);
    lv_obj_t * cl = lv_label_create(h);
    lv_label_set_text(cl, cbuf);
    lv_obj_add_style(cl, &st_text_mut, 0);
    lv_obj_set_style_text_font(cl, app_font_scaled(12), 0);
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
    lv_obj_set_size(b, w, SY(34));
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
    lv_obj_set_style_text_font(lb, app_font_scaled(12), 0);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    return b;
}

/* 单个用户行：[头像圆] [用户名(+临时副文案)] [弹性占位] [状态] [操作组] */
static void add_user_row(const safe_user_t * u, bool is_admin, bool last_admin)
{
    int uid = u->id;

    /* 行容器 */
    lv_obj_t * row = lv_obj_create(s_list);
    lv_obj_set_size(row, lv_pct(100), SY(56));
    lv_obj_add_style(row, &st_panel2, 0);
    lv_obj_set_style_radius(row, SX(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, SX(8), 0);
    lv_obj_set_style_pad_row(row, 0, 0);
    /* st_panel2 默认 pad_all=10：行高 56 只剩 36 可用，装不下「用户名 + 临时副文案」
     * 两行文字（≈42px）。压到上下各 4 → 48 可用，两行也放得下，行高保持统一。 */
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
    lv_obj_set_size(icon, SX(28), SX(28));
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
    lv_obj_set_style_text_font(initial_lbl, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(initial_lbl, is_admin ? theme_color(TH_ACCENT_INK) : theme_color(TH_TEXT), 0);
    lv_obj_center(initial_lbl);

    /* --- 用户名（+ 临时用户副文案） --- */
    lv_obj_t * mid = lv_obj_create(row);
    lv_obj_set_size(mid, SX(170), LV_SIZE_CONTENT);
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
    lv_obj_set_width(nm, SX(170));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);   /* 超长用户名截断，不撑破行高 */

    if (strcmp(u->role, "temp") == 0) {
        char sub[72];
        fmt_temp_sub(u, sub, sizeof(sub));
        lv_obj_t * sl = lv_label_create(mid);
        lv_label_set_text(sl, sub);
        lv_obj_add_style(sl, &st_text_mut, 0);
        lv_obj_set_style_text_font(sl, app_font_scaled(11), 0);
        lv_obj_set_width(sl, SX(170));
        lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
    }

    /* 弹性占位，把状态与操作组推到右侧 */
    lv_obj_t * sp1 = lv_obj_create(row);
    lv_obj_set_flex_grow(sp1, 1);
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
    lv_obj_set_width(st_lbl, SX(64));

    /* 小间距 */
    lv_obj_t * sp2 = lv_obj_create(row);
    lv_obj_set_size(sp2, SX(8), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);
    lv_obj_set_scrollable(sp2, false);

    /* --- 右侧操作组：改密 / 启停 / 人脸 / 删除 --- */
    lv_obj_t * ops = lv_obj_create(row);
    lv_obj_set_size(ops, LV_SIZE_CONTENT, SY(34));
    lv_obj_set_style_bg_opa(ops, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ops, 0, 0);
    lv_obj_set_style_pad_all(ops, 0, 0);
    lv_obj_set_style_pad_column(ops, SX(6), 0);
    lv_obj_set_flex_flow(ops, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ops, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ops, false);

    /* 改密 */
    row_op_btn(ops, UI_GLYPH_EDIT, "改密", SX(76), &st_ghost_btn, TH_TEXT,
               row_pwd_cb, uid);

    /* 启用/停用 —— 末位管理员锁死 */
    lv_obj_t * b_tog = row_op_btn(ops,
                                  u->enabled ? UI_GLYPH_LOCK : UI_GLYPH_LOCK_OPEN,
                                  u->enabled ? "停用" : "启用",
                                  SX(76), &st_ghost_btn, TH_TEXT, NULL, uid);
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
                                   SX(88), &st_ghost_btn,
                                   has_face ? TH_DANGER : TH_ACCENT, NULL, uid);
    if (!cap_ok) {
        lv_obj_add_state(b_face, LV_STATE_DISABLED);
        lv_obj_set_style_opa(b_face, LV_OPA_40, LV_STATE_DISABLED);
    } else {
        lv_obj_add_event_cb(b_face, row_face_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    }

    /* 删除 —— 末位管理员锁死 */
    lv_obj_t * b_del = row_op_btn(ops, UI_GLYPH_DELETE, "删除", SX(76), &st_danger_btn,
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
    /* ★ 遮罩是纯装饰层（半透明黑）：LVGL v9 的 lv_obj 默认可滚动，不关就能被拖动。
     * dlg_open() 是**所有 modal 的公共基座**（添加用户 / 改密 / OTP 共用）——
     * 之前只补了 s_auth_ov / s_otp_ov 这些具体容器，恰恰漏了基座本身。 */
    lv_obj_set_scrollable(s_ov, false);
    lv_obj_add_event_cb(s_ov, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, w, h);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    /* 窗口同样关滚动：各弹窗内部一律用 flex 排布 + 定高，从不依赖滚动；
     * 万一某天内容超高，应表现为「布局问题被看见」而不是「偷偷变成内部滚动、
     * 按钮跑到折叠线以下」（这正是 SYSTEM 页历史缺陷的形态）。 */
    lv_obj_set_scrollable(s_win, false);
    /* ★ 修复：弹窗顶部距屏顶 SY(10)，底部 ≈ 360+10=370；1.0x 板键盘从 y=380 起，
     *      留 10px 缝隙；1.8x PC 同样不挡。 */
    lv_obj_align(s_win, LV_ALIGN_TOP_MID, 0, SY(10));
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* ★ 修复：原 8 间距太小，叠加 st_panel 自带 pad_all=16，3 个 textarea+2 按钮共 5 行溢出窗口；改 6 */
    lv_obj_set_style_pad_row(s_win, SY(6), 0);

    /* ★ 每个弹窗重建时清空上一轮的控件句柄：弹窗种类不同（添加用户 5 个输入框 /
     * 改密 3 个），不清会在 save 回调里读到已被删除的悬垂指针。 */
    for (int i = 0; i < DLG_TA_MAX; i++)    s_dlg_tas[i] = NULL;
    for (int i = 0; i < ROLE_SEL_CNT; i++)  s_role_btns[i] = NULL;
    s_temp_row = NULL;

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

/* 弹窗输入框。parent 允许不是 s_win —— 「添加用户」把 有效期/次数上限 收在一行
 * 容器里（弹窗字段布局：同类字段同行，纵向不无限拉长）。 */
static lv_obj_t * dlg_textarea_ex(lv_obj_t * parent, const char *label, bool password,
                                  bool number, int idx, int32_t w)
{
    if (idx >= 0 && idx < DLG_TA_MAX) s_dlg_tas[idx] = NULL;  /* 先清空，防 dialog 复用残留 */
    lv_obj_t * ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, SY(40));
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
    } else if (number) {
        /* 数字非密码字段（有效期 / 次数上限）：限 4 位，防止手滑敲出一串数字 */
        lv_textarea_set_max_length(ta, 4);
    }
    lv_obj_add_event_cb(ta, kb_focus_cb, LV_EVENT_FOCUSED, (void *)(uintptr_t)(number ? 1 : 0));
    lv_obj_set_style_text_font(ta, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(ta, theme_color(TH_TEXT), 0);
    if (idx >= 0 && idx < DLG_TA_MAX) s_dlg_tas[idx] = ta;
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

static lv_obj_t * dlg_textarea(const char *label, bool password, bool number, int idx)
{
    return dlg_textarea_ex(s_win, label, password, number, idx, SX(300));
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
    /* 人脸操作的确认弹窗被取消：复位待办标记（对其它弹窗无副作用，恒为 NONE） */
    s_face_op = FACE_OP_NONE;
    s_face_pending_uid = -1;
    s_face_pending_tpl = -1;
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

/* 角色分段选择：三个按钮互斥，选中态走 st_accent_btn（与 SYSTEM 页主题列表同套路，
 * 颜色随主题联动，无需注册 theme 刷新回调）。 */
static void dlg_role_row(void)
{
    lv_obj_t * row = lv_obj_create(s_win);
    lv_obj_set_size(row, SX(300), SY(34));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SX(6), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    for (int i = 0; i < ROLE_SEL_CNT; i++) {
        lv_obj_t * b = lv_button_create(row);
        lv_obj_set_size(b, SX(94), SY(32));
        lv_obj_add_style(b, &st_ghost_btn, 0);
        lv_obj_add_style(b, &st_accent_btn, LV_STATE_CHECKED);
        lv_obj_set_style_pad_hor(b, SX(4), 0);
        lv_obj_set_style_pad_ver(b, 0, 0);
        lv_obj_set_style_radius(b, SX(8), 0);
        lv_obj_add_event_cb(b, role_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, ROLE_TEXT[i]);
        lv_obj_set_style_text_font(l, app_font_scaled(13), 0);
        lv_obj_center(l);
        s_role_btns[i] = b;
    }
}

/* 临时授权限制行（FR-9 创建入口）：有效期 + 次数上限。
 * 两项可只设其一、留空即不限（规约 §5.6）；用「天数」而不是日期时间选择器 ——
 * 触屏上数字键盘输入天数比点日期快一个数量级，且不会出现「过去的日期」这类脏输入。 */
static void dlg_temp_row(void)
{
    lv_obj_t * row = lv_obj_create(s_win);
    lv_obj_set_size(row, SX(300), SY(40));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SX(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    s_temp_row = row;

    dlg_textarea_ex(row, "有效期(天)", false, true, 3, SX(146));
    dlg_textarea_ex(row, "次数上限",   false, true, 4, SX(146));
}

/* 底部按钮行：保存 / 取消 同行并排（原来上下堆叠多占一行，弹窗被迫拉高，
 * 在 1.0x 板端会顶到软键盘）。 */
static void dlg_btn_row(void)
{
    lv_obj_t * row = lv_obj_create(s_win);
    lv_obj_set_size(row, SX(300), SY(44));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SX(10), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    lv_obj_t * save = ui_icon_text_button(row, LV_SYMBOL_SAVE, "保存",
                                          SX(150), SY(44), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), save_add_cb, NULL);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);

    /* 取消按钮 — ★ 不用 st_ghost_btn（实测该样式的 text_color 有时不级联到 label，
     *  "取消"会显示成深色方块），改为手动设置所有样式 */
    lv_obj_t * canc = lv_button_create(row);
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

static void role_btn_cb(lv_event_t * e)
{
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= ROLE_SEL_CNT) return;
    s_role_sel = (role_sel_t)i;
    dlg_sync_role();
}

/* 角色切换的联动：刷新三颗分段按钮的选中态，并只让「临时用户」显示限制行。
 * 顺带把键盘焦点收回 PIN 框 —— 否则焦点留在刚被隐藏的输入框上，键盘会继续往
 * 一个看不见的框里打字。 */
static void dlg_sync_role(void)
{
    for (int i = 0; i < ROLE_SEL_CNT; i++) {
        if (!s_role_btns[i]) continue;
        if (i == (int)s_role_sel) lv_obj_add_state(s_role_btns[i], LV_STATE_CHECKED);
        else                      lv_obj_remove_state(s_role_btns[i], LV_STATE_CHECKED);
    }
    if (s_temp_row) lv_obj_set_hidden(s_temp_row, s_role_sel != ROLE_SEL_TEMP);
    /* 提示行复用弹窗的 msg 槽：选到临时用户时直接把「留空=不限」写清楚，
     * 免得两个输入框的占位文字被挤成省略号后语义不明。 */
    if (s_msg) {
        lv_label_set_text(s_msg, (s_role_sel == ROLE_SEL_TEMP)
                                 ? "临时用户：两项可只设其一，留空即不限"
                                 : " ");
    }
    if (s_kb && s_dlg_tas[1]) {
        lv_keyboard_set_textarea(s_kb, s_dlg_tas[1]);
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    }
}

static void dlg_add_user(void)
{
    s_role_sel = ROLE_SEL_USER;
    /* 高度 SY(330)：内容实测 ≈ 290（含 st_panel 的 pad_all=16），
     * 顶部 SY(10) + 330 = 340 < 600 - 220(软键盘)，不会被键盘压住。 */
    lv_obj_t * win = dlg_open("添加用户", SX(380), SY(330));  /* 添加用户 */
    (void)win;
    dlg_textarea("用户名(字母数字)", false, false, 0);
    dlg_textarea("PIN(4-8 位数字)",   true,  true,  1);  /* 默认焦点 */
    dlg_textarea("确认 PIN",          true,  true,  2);
    dlg_role_row();
    dlg_temp_row();
    dlg_btn_row();
    dlg_sync_role();
}

/* 递归收集弹窗内的 textarea（创建顺序 = 期望的字段顺序）。
 * 为什么要递归：「添加用户」把最后两个字段收进了一行容器，它们不再是 s_win 的
 * 直接子对象，只扫一层会漏掉。textareas 自身不再往下钻（它的内部 label 不是输入框）。 */
static void dlg_collect_tas(lv_obj_t * parent, lv_obj_t * tas[], int max, int * ti)
{
    uint32_t cnt = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < cnt && *ti < max; i++) {
        lv_obj_t * c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_textarea_class)) {
            tas[(*ti)++] = c;
        } else if (lv_obj_get_child_count(c) > 0) {
            dlg_collect_tas(c, tas, max, ti);
        }
    }
}

static int dlg_get_tas(lv_obj_t * tas[], int max)
{
    int ti = 0;
    dlg_collect_tas(s_win, tas, max, &ti);
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
    /* 角色由弹窗的分段按钮给出；这里用 store 的 user_role_valid() 再兜一层 ——
     * UI 与 worker 各判一次，任何一条写路径都不会把脏 role 落进用户表
     * （FR-1 角色表 / FR-9；非法值退回 "user" 而不是拒绝，避免界面与存储不一致）。 */
    strncpy(u.role, user_role_valid(a->role) ? a->role : "user", sizeof(u.role) - 1);
    strncpy(u.auth_method, "pin", sizeof(u.auth_method) - 1);
    u.enabled = true;
    /* 新建用户默认未绑定人脸模板：face_id 必须显式置 -1（memset 给的是 0，
     * 而 UI 用人脸按钮按 face_id>=0 判定「已录入」，不置 -1 会让新用户错显示
     * 「删人脸」且被 user_find_by_face(0) 误命中）。rpc.c 的 add_user 同样置 -1。 */
    u.face_id = -1;
    /* 临时授权（FR-9）：两项可为 0（不限），也能只设其一 —— 到期/耗尽的自动删除
     * 由 auth_fsm 在任一认证通道命中时执行，这里只负责把限制写进去。 */
    u.valid_until = a->valid_until;
    u.use_limit   = a->use_limit;
    u.used_count  = 0;
    uint8_t salt[16];
    pin_hash(a->pin, salt, u.pin_hash);
    salt_to_hex(salt, u.pin_salt);
    time_t now = (time_t)hal_time();
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(u.created_at, sizeof(u.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);

    a->result = user_add(&u);
    if (a->result == 0) {
        char det[96];
        /* 审计详情带上角色与限制：事后回看「这个临时用户当初给的是几天几次」
         * 是审计的基本诉求（FR-5 / FR-9）。 */
        if (strcmp(u.role, "temp") == 0) {
            snprintf(det, sizeof(det), "%s role=%s until=%lld limit=%d",
                     u.name, u.role, (long long)u.valid_until, u.use_limit);
        } else {
            snprintf(det, sizeof(det), "%s role=%s", u.name, u.role);
        }
        log_append("user_add", "admin", 1, det);
    }
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
    lv_obj_t * tas[DLG_TA_MAX];
    int ti = dlg_get_tas(tas, DLG_TA_MAX);
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

    /* 角色（FR-9 创建入口）：分段按钮的选中项即角色，交 store 的合法性校验把关 */
    if (s_role_sel < 0 || s_role_sel >= ROLE_SEL_CNT) {
        dlg_set_msg("角色非法"); return;
    }
    const char * role = ROLE_KEY[s_role_sel];
    if (!user_role_valid(role)) { dlg_set_msg("角色非法"); return; }

    /* 临时授权的两项限制：可只设其一，留空即不限（规约 §5.6）。
     * 非临时用户直接置 0，杜绝「角色改成普通用户、限制却还留着」的脏数据。 */
    int64_t valid_until = 0;
    int     use_limit   = 0;
    if (s_role_sel == ROLE_SEL_TEMP) {
        const char * ds = (ti > 3 && tas[3]) ? lv_textarea_get_text(tas[3]) : "";
        const char * ls = (ti > 4 && tas[4]) ? lv_textarea_get_text(tas[4]) : "";
        if (ds[0]) {
            int days = atoi(ds);
            if (days <= 0 || days > 365) { dlg_set_msg("有效期需 1-365 天"); return; }
            valid_until = (int64_t)hal_time() + (int64_t)days * 86400;
        }
        if (ls[0]) {
            int lim = atoi(ls);
            if (lim <= 0 || lim > 9999) { dlg_set_msg("次数上限需 1-9999 次"); return; }
            use_limit = lim;
        }
    }

    add_user_job_t * a = (add_user_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->pin, pin1, sizeof(a->pin) - 1);
    strncpy(a->role, role, sizeof(a->role) - 1);
    a->valid_until = valid_until;
    a->use_limit   = use_limit;
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

    lv_obj_t * save = ui_icon_text_button(s_win, LV_SYMBOL_SAVE, "保存",
                                          SX(160), SY(44), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), save_pwd_cb, NULL);
    lv_obj_add_style(save, &st_accent_btn_pr, LV_STATE_PRESSED);
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
    /* 二次确认被放弃：人脸待办一并复位（删除用户路径该标记本就是 NONE） */
    s_face_op = FACE_OP_NONE;
    s_face_pending_uid = -1;
    s_face_pending_tpl = -1;
}

/* PBKDF2 校验在后台线程跑，结果回主线程 */
static void auth_result(int r)
{
    if (r == 0) {
        auth_close();
        /* ★ 分叉：本次二次确认是为哪个人脸操作发起的（UI 现代化 ui3） */
        if (s_face_op == FACE_OP_ENROLL) {
            dlg_confirm_face("确认为该用户录入人脸？\n录入期间请正对摄像头保持不动。", "开始录入");
            return;
        }
        if (s_face_op == FACE_OP_DELETE) {
            dlg_confirm_face("确定删除该用户已绑定的人脸？\n删除后该用户将无法刷脸开锁。", "确认删除");
            return;
        }
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

/* 管理员 PIN 二次确认弹窗。sub_text 说明「本次二次确认是为了哪类敏感操作」，
 * 因为删除用户 / 录入人脸 / 删除人脸 复用同一套弹窗（UI 现代化 ui3）。 */
static void auth_show_ctx(const char * sub_text)
{
    auth_close();
    lv_obj_t * scr = lv_screen_active();

    s_auth_ov = lv_obj_create(scr);
    lv_obj_set_size(s_auth_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_auth_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_auth_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_auth_ov, 0, 0);
    lv_obj_set_scrollable(s_auth_ov, false);
    lv_obj_add_event_cb(s_auth_ov, auth_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * win = lv_obj_create(s_auth_ov);
    lv_obj_set_size(win, SX(380), SY(430));
    lv_obj_add_style(win, &st_panel, 0);
    lv_obj_set_style_radius(win, SX(16), 0);
    lv_obj_set_style_pad_all(win, 0, 0);       /* 下面全用绝对坐标，去掉默认 padding */
    lv_obj_set_scrollable(win, false);         /* 同「头像圆」根因：装饰容器一律关滚动 */
    lv_obj_center(win);

    lv_obj_t * t = lv_label_create(win);
    lv_label_set_text(t, "管理员验证");
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    lv_obj_set_width(t, SX(380));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, SY(14));

    lv_obj_t * sub = lv_label_create(win);
    lv_label_set_text(sub, (sub_text && *sub_text) ? sub_text
                                                   : "删除用户需二次验证管理员 PIN");
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

    lv_obj_t * ok = ui_icon_text_button(win, LV_SYMBOL_OK, "确认",
                                         SX(160), SY(44), &st_accent_btn,
                                         theme_color(TH_ACCENT_INK), auth_ok_cb, NULL);
    lv_obj_set_pos(ok, SX(200), SY(376));
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
}

/* 兼容旧调用点：不指定副标题时按「删除用户」文案（原行为）。 */
static void auth_show(void)
{
    auth_show_ctx(NULL);
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

/* ================================================================
 *  确认弹窗底部按钮定位（dlg_confirm_del / dlg_confirm_face 共用）
 *
 *  ⚠ 陷阱：不能用 lv_obj_get_width/height(s_win) 反推按钮坐标 —— dlg_open 里
 *  刚 lv_obj_set_size 完的窗口要到【下一次布局】才有 coords，此处查询恒为 0，
 *  会让按钮落到负坐标（画不出来）。直接用 dlg_open 的入参尺寸算。
 *  返回的两个按钮：左「取消」在 x，右「确认」在 x+144，尺寸均 136×44。
 * ================================================================ */
static int32_t s_confirm_btn_y;         /* begin() 里按文案实际高度算好（堆叠修复） */

static void dlg_bottom_btn_xy(int win_w, int win_h, int32_t * x, int32_t * y)
{
    (void)win_h;                            /* 按钮位由 begin() 按文案实测决定 */
    *y = s_confirm_btn_y;                   /* 不再固定：见 dlg_confirm_begin 的堆叠修复 */
    *x = (win_w - 32 - 280) / 2 + 16;       /* 两按钮(136×2)+间距 8 共 280，左右各留 16 */
}

/* 确认弹窗骨架：文案顶对齐堆叠（否则列居中的文案会被底部按钮压住），
 * 键盘隐藏（本类弹窗无输入框）。win 已由 dlg_open 建好。 */
static void dlg_confirm_begin(int win_w, int win_h, const char * title, const char * msg)
{
    dlg_open(title, win_w, win_h);
    /* dlg_open 默认列居中：文案会被底部按钮压住，改成顶部对齐 */
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* dlg_open 会无条件建一张软键盘（供表单弹窗用）；本确认弹窗没有输入框，
     * 隐藏它以免键盘压住底部按钮（PC 1.8x 下会重叠，见截图验收）。 */
    if (s_kb) lv_obj_set_hidden(s_kb, true);
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, msg);
    lv_obj_add_style(m, &st_text, 0);
    lv_obj_set_style_text_font(m, app_font_scaled(16), 0);
    lv_obj_set_width(m, win_w - 32);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);

    /* 堆叠修复（用户验收 2026-09-15）：文案折行后底部会越过固定按钮位。
     * 强制布局量出文案实际底边，按钮位取「默认位 / 文案下方」较大者，并统一
     * 按内容定高、重新居中。update_layout 必须先做：dlg_open 刚 set_size 完时
     * 坐标查询恒为 0（见上方陷阱注释）。
     * 实测（第二轮截图验收）：量出的文案高度仍比最终渲染矮约一行（缩放字体
     * 度量在布局与首绘间有偏差），故额外留一行余量。 */
    lv_obj_update_layout(s_win);
    int32_t need_y  = lv_obj_get_y(m) + lv_obj_get_height(m) + 40;
    int32_t def_y   = win_h - 16 - 44 - 16;
    s_confirm_btn_y = (need_y > def_y) ? need_y : def_y;
    int32_t total_h = s_confirm_btn_y + 44 + 16;
    if (total_h < win_h) total_h = win_h;
    lv_obj_set_size(s_win, win_w, total_h);
    lv_obj_center(s_win);
}

/* ================================================================
 *  人脸录入 / 删除（UI 现代化 ui3，spec §4）
 *  链路：行按钮 → admin PIN 二次确认（与删除用户同套弹窗）
 *       → 确认弹窗 → face_service_enroll_async()/delete_async()（立即返回）
 *       → EV_FACE_EVENT(ENROLL_DONE/DELETE_DONE)
 *           ├─ ui_feedback：横幅反馈
 *           └─ 本页：user_face_set 写回模板号（worker 线程落盘）+ 刷新列表
 * ================================================================ */

/* 人脸操作确认弹窗（复用 dlg_confirm_del 的版式，按钮语义换成 开始/取消） */
static void dlg_confirm_face(const char * msg, const char * ok_text)
{
    const int32_t win_w = 380, win_h = 200;
    dlg_confirm_begin(win_w, win_h, "人脸操作确认", msg);

    int32_t btn_x = 0, btn_y = 0;
    dlg_bottom_btn_xy(win_w, win_h, &btn_x, &btn_y);

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 136, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_set_floating(cc, true);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font_scaled(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * yy = ui_icon_text_button(s_win, LV_SYMBOL_OK, ok_text,
                                        136, 44, &st_accent_btn,
                                        theme_color(TH_ACCENT_INK),
                                        face_start_cb, NULL);
    lv_obj_set_floating(yy, true);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
}

/* 行内「录/删人脸」按钮：记下操作类型后进 admin PIN 二次确认 */
static void row_face_cb(lv_event_t * e)
{
    s_sel_id = (int)(uintptr_t)lv_event_get_user_data(e);
    face_btn_cb(e);
}

static void face_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    safe_user_t u;
    if (user_find_by_id(s_sel_id, &u) != 0) {
        dlg_tip("用户不存在，请刷新列表");
        return;
    }
    /* 后端能力以点击时为准（构建期 none 后端按钮已置灰，这里再兜一层） */
    if (!face_cap_ok(u.face_id < 0)) {
        dlg_tip("当前人脸后端不支持该操作");
        return;
    }
    s_face_op          = (u.face_id >= 0) ? FACE_OP_DELETE : FACE_OP_ENROLL;
    s_face_pending_uid = u.id;
    s_face_pending_tpl = u.face_id;
    /* ★ 敏感操作：与删除用户同一套 admin PIN 二次确认，副标题按本次操作定制 */
    auth_show_ctx(s_face_op == FACE_OP_DELETE ? "删除人脸需二次验证管理员 PIN"
                                              : "录入人脸需二次验证管理员 PIN");
}

/* 二次确认通过：真正发起异步录入/删除 */
static void face_start_cb(lv_event_t * e)
{
    (void)e;
    safe_err_t r;
    if (s_face_op == FACE_OP_ENROLL) {
        /* v1.4（FR-21 防线 1）：把用户名字带进模组，使 VERIFY 应答能带回该名做凭据核对。
         * 这里读一次 store 属点击路径（非认证路径），与列表刷新同源；查不到就传空名
         * —— 空名只会让核对跳过，不影响录入本身。 */
        char uname[32] = {0};
        safe_user_t uu;
        if (s_face_pending_uid >= 0 && user_find_by_id(s_face_pending_uid, &uu) == 0) {
            strncpy(uname, uu.name, sizeof(uname) - 1);
        }
        r = face_service_enroll_async(uname);     /* hal_face.h，立即返回 */
    } else if (s_face_op == FACE_OP_DELETE) {
        r = face_service_delete_async(s_face_pending_tpl);   /* hal_face.h:107 */
    } else {
        return;
    }
    close_dlg();
    if (r != SAFE_OK) {
        /* 发起就失败（设备忙/不支持）：不会收到 DONE 事件，这里就地复位并提示 */
        char buf[96];
        snprintf(buf, sizeof(buf), "人脸操作发起失败（err=%d）", (int)r);
        dlg_tip(buf);
        s_face_op = FACE_OP_NONE;
        s_face_pending_uid = -1;
        s_face_pending_tpl = -1;
    }
    else if (s_face_op == FACE_OP_ENROLL) {
        s_last_enroll_uid = s_face_pending_uid;
        /* 录入是独立窗口（用户拍板 2026-09-15：录入引导与识别分开）——切到
         * 专用「人脸录入」页（视频 + 实时引导），识别仍留在「人脸识别」页。
         * 先清掉上一轮可能残留的结果态（成功/失败文案与「重新录入」按钮）。 */
        page_enroll_show_result(NULL, false);
        ui_switch_page(PAGE_ENROLL);
    }
    /* 发起成功：等 EV_FACE_EVENT 应答，由 on_face_event_ui 写回 + 刷新 */
}

/* 录入页「重新录入」入口（用户拍板 2026-09-15：失败不弹窗、不切页，就地重试）。
 * 应答处理（on_face_event_ui）已把 s_face_op / s_face_pending_uid 复位，这里按
 * s_last_enroll_uid 重新装填上下文并重发一次录入命令，使下一次 ENROLL_DONE
 * 仍能被本页接住并写回 face_id。成功返回 true；模组忙等不受理返回 false。 */
bool page_users_retry_enroll(void)
{
    if (s_last_enroll_uid < 0) return false;

    /* 与 face_start_cb 同一套发起路径：把用户名带进模组，供 VERIFY 时核对归属 */
    char uname[32] = {0};
    safe_user_t uu;
    if (user_find_by_id(s_last_enroll_uid, &uu) == 0) {
        strncpy(uname, uu.name, sizeof(uname) - 1);
    }

    s_face_op          = FACE_OP_ENROLL;
    s_face_pending_uid = s_last_enroll_uid;
    s_face_pending_tpl = -1;

    if (face_service_enroll_async(uname) != SAFE_OK) {
        s_face_op          = FACE_OP_NONE;
        s_face_pending_uid = -1;
        return false;
    }
    return true;
}

/* 人脸操作失败原因 → 可操作提示（v1.4 / N3）。
 * 后端现在按模组 MR_* 码给出可区分的 safe_err_t（如 SAFE_ERR_EXIST = 该脸已录入），
 * UI 才有话可说；此前一律坍缩成 SAFE_ERR_FAIL，界面只能说「失败」，用户不知道下一步做什么。 */
static const char * face_err_hint(bool enroll, safe_err_t e)
{
    switch (e) {
        case SAFE_ERR_EXIST:   return enroll ? "这张脸已经录入过（同一张脸只能绑定一个用户）"
                                             : "模组侧未完成删除，请重试";
        case SAFE_ERR_NOMEM:   return enroll ? "模组模板已满，请先删除不需要的人脸"
                                             /* 删除被拒却报容量问题，属模组侧异常；
                                              * 光说「内存不足」用户无从下手，给出下一步。 */
                                             : "模组容量计数异常（删除被拒）：请重启模组后重试";
        case SAFE_ERR_PARAM:   return "模组拒绝了请求参数";
        case SAFE_ERR_TIMEOUT: return enroll ? "录入超时：没有检测到人脸，请正对镜头"
                                             : "模组响应超时";
        case SAFE_ERR_NOENT:   return "模组侧没有该模板（可能已被清除）";
        case SAFE_ERR_BUSY:    return "模组忙碌，请稍后重试";
        case SAFE_ERR_STATE:   return "模组拒绝了该操作（当前状态不允许）";
        default:               return enroll ? "录入失败，请重试" : "删除失败，请重试";
    }
}

/* face_id 写回 users.json —— 文件 IO 下沉 worker 线程，主线程不阻塞（CLAUDE.md §8） */
static void face_set_worker(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    a->result = user_face_set(a->id, a->result);   /* 复用 result 传模板号（-1=清除） */
}

static void face_set_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result != 0) {
        dlg_tip("人脸绑定写回失败，请重试");
    }
    free(a);
    rebuild_list();   /* 应答到达后重载该用户条目（现有列表刷新路径，spec §4） */
}

/* EV_USER_CHANGED：**别处**改动了用户数据 → 本页重新拉一次列表。
 *
 * 为什么需要：本页原先只在「进入页面」与「自己发起操作」后 reload，别处改的数据
 * **不会反映到这里**。用户实测（2026-09-20）：在 SYSTEM 页清空模组人脸、本地绑定
 * 已被同步清掉之后，回到用户页仍看到 admin 挂着「删人脸」按钮。
 *
 * 注：EV_USER_CHANGED 此前被判定为「全工程无订阅者的死代码」（QA 低危#3），
 * 从本次起它有了真实用途（本页 + 启动对账两处广播）。 */
static void on_user_changed(ev_topic_t topic, const void * payload, void * user)
{
    (void)payload; (void)user;
    if (topic != EV_USER_CHANGED) return;
    astore_load_users(users_list_loaded);
}

/* EV_FACE_EVENT 应答：录入成功写回模板号，删除成功写 -1，然后刷新列表 */
static void on_face_event_ui(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if (topic != EV_FACE_EVENT || payload == NULL) return;

    const ev_face_event_t * e = (const ev_face_event_t *)payload;
    int tpl = -1;
    safe_err_t fail_err = SAFE_OK;                    /* v1.4：失败原因，用于给可操作提示 */
    bool was_enroll = (s_face_op == FACE_OP_ENROLL);

    if (e->ev == FACE_EV_ENROLL_DONE) {
        if (s_face_op != FACE_OP_ENROLL) return;      /* 不是本页发起的（如 RPC），只出横幅 */
        if (e->enroll.err != SAFE_OK) { tpl = -2; fail_err = e->enroll.err; }  /* 失败：不写回 */
        else tpl = e->enroll.face_id;
    } else if (e->ev == FACE_EV_DELETE_DONE) {
        if (s_face_op != FACE_OP_DELETE) return;
        /* ★ 幂等语义（2026-09-20）：模组回 NOENT 表示**它那儿本来就没有这个
         *   模板**，而调用方的意图是「解绑」—— 目的已经达成，应当视为成功并
         *   清除本地绑定。
         *   否则会**死锁**：本地留着 face_id、模组里没有对应模板，用户想解绑
         *   却每次都「删除失败」，只能等启动对账兜底（而对账只在启动时跑一次）。
         *   这正是用户实测场景：「app 态存过人脸，但模组里已删掉」。 */
        if (e->del.err == SAFE_ERR_NOENT)      tpl = -1;   /* 模组无此模板 = 已达成 */
        else if (e->del.err != SAFE_OK)      { tpl = -2; fail_err = e->del.err; }
        else                                   tpl = -1;   /* 正常删除成功 */
    } else {
        return;
    }

    int uid = s_face_pending_uid;
    s_face_op = FACE_OP_NONE;
    s_face_pending_uid = -1;
    s_face_pending_tpl = -1;

    if (tpl == -2 || uid < 0) {
        /* 横幅由 ui_feedback 出（只说「失败」）；这里补一句能指导下一步操作的提示。
         * 用户拍板 2026-09-15：录入失败**不要再弹 modal 对话框**，文案直接送到
         * 录入页底部状态行，页面停在录入页让人对着引导重试（按钮变「重新录入」）。
         * 删除失败仍走用户页 dlg_tip，行为不变。 */
        if (was_enroll) {
            page_enroll_show_result(face_err_hint(true, fail_err), false);
        } else if (fail_err != SAFE_OK) {
            dlg_tip(face_err_hint(false, fail_err));
        }
        return;
    }

    user_op_job_t * a = (user_op_job_t *)calloc(1, sizeof(*a));
    if (a == NULL) return;
    a->id = uid;
    a->result = tpl;                                   /* worker 内转成 user_face_set 的入参 */
    worker_post(face_set_worker, a, face_set_done);
    if (was_enroll) {
        /* 录入成功：同一接口在录入页显示一句「录入成功」，再回用户页看结果 */
        page_enroll_show_result("录入成功", true);
        ui_switch_page(PAGE_USERS);
    }
}

static void dlg_confirm_del(const char *msg)
{
    const int32_t win_w = 360, win_h = 200;
    dlg_confirm_begin(win_w, win_h, "删除确认", msg);

    /* ⚠ 原实现用 lv_obj_get_width/height(s_win) 反推坐标，新建窗口尚未布局、
     *    查询恒为 0 → 按钮落到负坐标不可见（预存在缺陷，ui3 一并修复）。 */
    int32_t btn_x = 0, btn_y = 0;
    dlg_bottom_btn_xy(win_w, win_h, &btn_x, &btn_y);

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 136, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_set_floating(cc, true);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font_scaled(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * yy = ui_icon_text_button(s_win, LV_SYMBOL_TRASH, "删除",
                                        136, 44, &st_danger_btn,
                                        lv_color_white(), do_del_cb, NULL);
    lv_obj_set_floating(yy, true);
    lv_obj_set_pos(yy, btn_x + 144, btn_y);
}

static void dlg_tip(const char *text)
{
    const int32_t win_w = 340;
    dlg_open("提示", win_w, 150);

    /* ★ 与 dlg_confirm_begin 同一手法（用户 2026-09-20 截图验收：标题、文案、
     *   按钮「黏成一坨」，且标题被上边裁、按钮被下边裁）。
     *   根因：这里用**固定 150 高**且未按内容定高，而 dlg_open 默认是列居中 ——
     *   内容一旦超过 150 就上下双向溢出（不是只往下长）。PC 1.8x 缩放下
     *   「标题栏 + app_font_scaled(16) 文案 + 40 高按钮」本来就装不进 150。
     *   修法：改顶部对齐 + 文案限宽折行 + 实测按钮底边后重新定高居中。 */
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t * m = lv_label_create(s_win);
    lv_label_set_text(m, text);
    lv_obj_add_style(m, &st_warn_text, 0);
    lv_obj_set_style_text_font(m, app_font_scaled(16), 0);
    lv_obj_set_width(m, win_w - 32);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t * btn = ui_icon_text_button(s_win, LV_SYMBOL_OK, "知道了",
                                         120, 40, &st_ghost_btn,
                                         theme_color(TH_TEXT), dlg_ok_cb, NULL);

    /* 按内容定高：文案折行会让按钮下移，量出实际底边再定高并重新居中。
     * 必须先 update_layout —— 刚 set_size 完的窗口坐标查询恒为 0（见
     * dlg_bottom_btn_xy 上方的陷阱注释，同一个坑）。 */
    lv_obj_update_layout(s_win);
    int32_t need_h = lv_obj_get_y(btn) + lv_obj_get_height(btn) + 16;
    int32_t h = (need_h > 150) ? need_h : 150;
    lv_obj_set_size(s_win, win_w, h);
    lv_obj_center(s_win);
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

/* FR-21 防线 2「确认删除结果」：本地用户记录删掉之后，模组侧的人脸模板也必须删掉，
 * 否则会在模组里留下孤儿模板，只能等启动对账兜底 —— 这与「删除即确认模组删除」不符。
 *
 * 为什么不复用「删人脸」对话框那条路径（face_btn_cb → FACE_OP_DELETE →
 * face_start_cb → on_face_event_ui）：那条路径的目标是**用户还在**时解绑模板，
 * 应答回来要写回 face_id=-1 并刷新列表；而这里用户记录已经删掉了，
 * 没有可写回的对象 —— 复用会让 on_face_event_ui 对一个已不存在的 uid 发
 * user_face_set。故此处只复用「face_service_delete_async + 审计」这一层语义，
 * 不再走 s_face_op 状态机，DELETE_DONE 事件会被 on_face_event_ui 正常忽略
 * （它要求 s_face_op == FACE_OP_DELETE 才处理）。 */
static void del_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        if (a->del_face_id >= 0 && (face_service_caps()->caps & FACE_CAP_DELETE)) {
            safe_err_t r = face_service_delete_async(a->del_face_id);
            char buf[128];
            if (r == SAFE_OK) {
                snprintf(buf, sizeof(buf), "删除用户：已下发模组删除模板 %d", a->del_face_id);
                astore_append_log("face_del", "admin", 1, buf);
            } else {
                /* 下发失败即按 FR-21「失败即本地标失效」处理：本地记录已删（凭据自然
                 * 失效），孤儿模板留给启动对账（FR-21 防线 3）回收，此处记审计告警。 */
                snprintf(buf, sizeof(buf),
                         "删除用户：模组删除模板 %d 失败（err=%d），留待启动对账处理",
                         a->del_face_id, (int)r);
                astore_append_log("ALARM", "admin", 0, buf);
            }
        }
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
    /* 模板号在这里（主线程、点击路径、用户记录还在的时候）取一次放进作业，
     * 供 del_done 做 FR-21 模组侧联动删除。取不到用户 / 该用户没绑人脸时为 -1。
     * 主线程读一次 store 与 face_btn_cb 的既有做法一致（点击路径，非认证路径）。 */
    a->del_face_id = -1;
    {
        safe_user_t du;
        if (s_sel_id >= 0 && user_find_by_id(s_sel_id, &du) == 0) a->del_face_id = du.face_id;
    }
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
    lv_obj_set_scrollable(s_otp_ov, false);
    lv_obj_add_event_cb(s_otp_ov, pwd_otp_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * win = lv_obj_create(s_otp_ov);
    lv_obj_set_size(win, SX(380), SY(430));
    lv_obj_add_style(win, &st_panel, 0);
    lv_obj_set_style_radius(win, SX(16), 0);
    lv_obj_set_style_pad_all(win, 0, 0);       /* 下面全用绝对坐标，去掉默认 padding */
    lv_obj_set_scrollable(win, false);         /* 同「头像圆」根因：装饰容器一律关滚动 */
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

    lv_obj_t * ok = ui_icon_text_button(win, LV_SYMBOL_OK, "验证",
                                         SX(160), SY(44), &st_accent_btn,
                                         theme_color(TH_ACCENT_INK), pwd_otp_ok_cb, NULL);
    lv_obj_set_pos(ok, SX(200), SY(376));
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
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

/* ★ 测试钩子：SAFE_TEST_DLG=face 直接打开"人脸操作确认"弹窗（UI 现代化 ui3 截图验收）。
 * 走「未录入 → 录人脸」分支的文案；只铺弹窗，不发起真实录入。 */
void page_users_test_open_face_dlg(void)
{
    s_face_op          = FACE_OP_ENROLL;
    s_face_pending_uid = 0;
    s_face_pending_tpl = -1;
    dlg_confirm_face("确认为该用户录入人脸？\n录入期间请正对摄像头保持不动。", "开始录入");
}


















