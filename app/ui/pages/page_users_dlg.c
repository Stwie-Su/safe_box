/**
 * @file page_users_dlg.c
 * 用户管理 — 弹窗族与业务操作（D24 自 page_users.c 拆出，纯搬移）：
 * 通用弹窗基建（overlay/键盘/textarea）、添加用户、修改信息（管理员
 * 改密走 TOTP 二次确认）、启停、删除与 admin PIN 二次确认、人脸录入/
 * 删除（EV_FACE_EVENT 写回）、面向 debug_hooks 的 test_open_*。
 */
#include "page_users.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/icons.h"               /* icon_label_colored：图标一律走图标字体 + theme token */
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/store/store_cache.h"     /* 主线程读用户快照（不直连 store，满足门禁） */
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "core/event_bus.h"
#include "core/auth/auth_verify.h"   /* auth_admin_verify_totp：改管理员密码动态码二次确认 */
#include "hal/hal_face.h"               /* hal_face_enroll/delete_async + caps（UI 现代化 ui3） */
#include "hal/hal_time.h"               /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include "ui/pages/page_enroll.h"       /* page_enroll_show_result：录入结果页内展示 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "page_users_internal.h"


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
static bool s_pwd_is_admin    = false;   /* 当前操作目标是否为管理员 */
static char s_cur_name[32]    = {0};     /* 弹窗打开时的原始用户名，用于判定是否改名 */

/* ---- 「修改信息」弹窗（FR-9 v1.11：可改角色与临时授权限制）----
 * s_auth_for_modify：本次管理员 PIN 二次确认是**为修改信息**发起的（auth_result 里分叉）。
 *   与 s_face_op 是同一套模式：二次确认弹窗是共用的，得知道「验完去干哪件事」。
 * s_pending_mod：跨二次确认弹窗要透传的修改请求（异步结果回来时原弹窗已关）。
 * s_ta_*：弹窗输入框的**显式句柄**。原先靠 dlg_get_tas() 按子对象创建顺序收集，
 *   字段一多（现在 6 个）顺序就极易错位且不报错 —— 保存时会把次数写进 PIN。 */
static bool              s_auth_for_modify = false;
static modify_user_req_t s_pending_mod;

static lv_obj_t * s_ta_name  = NULL;   /* 姓名 */
static lv_obj_t * s_ta_old   = NULL;   /* 旧 PIN */
static lv_obj_t * s_ta_new   = NULL;   /* 新 PIN */
static lv_obj_t * s_ta_new2  = NULL;   /* 确认新 PIN */
static lv_obj_t * s_ta_days  = NULL;   /* 有效期（天），仅临时用户 */
static lv_obj_t * s_ta_limit = NULL;   /* 次数上限，仅临时用户 */

static char      s_cur_role[16] = {0};   /* 弹窗打开时的角色，用于判定「是否真改了角色」 */

/* 打开弹窗时该用户的临时授权现值（预填用，来自 store_cache 快照） */
static int64_t   s_edit_until = 0;
static int       s_edit_limit = 0;
static int       s_edit_used  = 0;



static lv_obj_t * s_otp_ov    = NULL;
static lv_obj_t * s_otp_disp  = NULL;
static lv_obj_t * s_otp_msg   = NULL;
static char s_otp_pin[8]      = {0};

/* ---- 人脸录入/删除入口（UI 现代化 ui3）----
 * 敏感操作走与「删除用户」同一套 admin PIN 二次确认，因此需要记录
 * 「本次二次确认是为了哪个人脸操作」，auth_result 通过后再分叉。
 * 录入/删除本身是异步的（hal_face_*_async 立即返回），结果经
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

/* ---- 文件内前向声明（D24 拆分：原前向声明区中归本文件的条目；
 *      跨文件符号见 page_users_internal.h，不在此重复声明）---- */
static void face_start_cb(lv_event_t * e);
static void face_set_worker(void * p);
static void face_set_done(void * p);
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
static void dlg_btn_row(lv_event_cb_t save_cb);
static void dlg_sync_role(void);
static void role_btn_cb(lv_event_t * e);
static void kb_focus_cb(lv_event_t * e);
static void save_add_cb(lv_event_t * e);
static void save_modify_cb(lv_event_t * e);
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
static void modify_done(int result);
static void post_modify_user(const modify_user_req_t * r);

/* 后台 worker 类型 */
typedef struct {
    char    name[32];
    char    pin[16];
    char    role[16];       /* "admin" / "user" / "temp"（FR-9 创建入口） */
    int64_t valid_until;    /* 临时用户有效期截止（Unix 秒，0 = 不限） */
    int     use_limit;      /* 临时用户开锁次数上限（0 = 不限） */
    int     result;
} add_user_job_t;
/* 修改用户不再自己定义作业结构：改名 / 改 PIN / 改角色 / 改临时限制统一走
 * async_store 的 modify_user_req_t（见 astore_modify_user 注释），
 * 目的就是「一次落盘」——早期版本先改名落盘再验旧 PIN，旧 PIN 错时名字已经改了。 */
/* del_face_id：仅「删除用户」路径使用 —— 待联动删除的模组侧模板号（-1 = 无需联动）。
 * 放在这里是因为 user_del 之后本地记录就没了，模板号必须在删之前取好。 */
typedef struct { int id; int result; int del_face_id; } user_op_job_t;

/* （D24 拆分的遗留：下面 4 个函数在 page_users_rows.c 里各有自己的 static 定义，
 * 本文件的这几行前向声明是拆文件时没清干净的死代码 —— 声明了却永不定义，
 * 每次编译都刷 4 条 -Wunused-function。2026-10-09 清理。） */
static void add_user_worker(void * p);
static void add_user_done(void * p);
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

    /* ★ 布局优化（2026-09-21）：窗口高度改为【内容自适应】 ——
     * 原先 h 由各调用方写死（添加用户 330 / 修改信息 470 / 提示 150），往表单里
     * 加一个字段就要记得同步改数字，忘了就把内容裁掉（窗口不可滚动）。
     * 现在按内容定高，上限仍是「键盘上方可用区」，保证永远完整落在键盘之上
     *（键盘是后建子对象、绘制在窗口之上，越界部分点不到）。 */
    const int32_t kb_h  = SY(220);
    const int32_t scr_h = (int32_t)lv_obj_get_height(lv_screen_active());
    const int32_t avail = scr_h - kb_h - SY(10) - SY(10);   /* 上边距10 + 与键盘间隙10 */

    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_win, avail, 0);
    (void)h;   /* 兼容既有调用方签名；高度已由内容决定 */
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    /* ★ 2026-10-09 改为【可滚动】——这里是在推翻上面的旧注释，理由要写清楚：
     *   「修改信息」自 FR-9 v1.11 起从 4 个字段涨到 6 个（姓名 / 旧 PIN / 新 PIN /
     *   确认 / 角色分段 / 临时限制行），内容高度已超过 max_height（avail，板端
     *   ≈360px）。不滚动的后果不是「布局问题被看见」，而是**保存按钮被裁到窗口外
     *   根本点不到**——功能直接不可用，比悄悄滚动严重得多。折中：保留 max_height
     *   夹紧（窗口不会顶到键盘下面），超高时允许纵向滚动，按钮至少可达。
     *   「添加用户」等短表单内容不超高，开着滚动也毫无影响。 */
    lv_obj_set_scrollable(s_win, true);
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
    /* 「修改信息」的显式句柄同样要清：不清会在 save 回调里读到上一轮弹窗
     * （已被 lv_obj_delete）的悬垂指针 —— 与上面 s_dlg_tas 是同一个坑。 */
    s_ta_name = NULL; s_ta_old = NULL; s_ta_new = NULL; s_ta_new2 = NULL;
    s_ta_days = NULL; s_ta_limit = NULL;

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
    lv_obj_set_size(ta, w, SY(44));
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
void add_btn_cb(lv_event_t * e)
{
    (void)e;
    dlg_add_user();
}

/* 角色分段选择：三个按钮互斥，选中态走 st_accent_btn（与 SYSTEM 页主题列表同套路，
 * 颜色随主题联动，无需注册 theme 刷新回调）。 */
static void dlg_role_row(void)
{
    lv_obj_t * row = lv_obj_create(s_win);
    lv_obj_set_size(row, SX(300), SY(44));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SX(6), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    for (int i = 0; i < ROLE_SEL_CNT; i++) {
        lv_obj_t * b = lv_button_create(row);
        lv_obj_set_size(b, SX(94), SY(44));
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
    lv_obj_set_size(row, SX(300), SY(44));
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
/* 弹窗底部「保存 + 取消」行。save_cb 由调用方传入（添加用户 / 修改信息共用）。
 * ★ 带取消按钮：与「遮罩点击关闭」并不重复 —— 遮罩关闭是手势兜底，
 *   明确的取消按钮是可见的退出路径（原「修改信息」只有保存，属遗漏）。 */
static void dlg_btn_row(lv_event_cb_t save_cb)
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

    /* save_cb 由调用方给：「添加用户」与「修改信息」共用这一行按钮，
     * 只有保存回调不同（见 dlg_add_user / dlg_change_pwd）。 */
    lv_obj_t * save = ui_icon_text_button(row, LV_SYMBOL_SAVE, "保存",
                                          SX(150), SY(44), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), save_cb, NULL);
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
    dlg_btn_row(save_add_cb);
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
        /* 本地留痕 + 远程上报成对出现：审计的权威记录在 safe.log，远程通道只是
         * 把它送出去（上报失败不影响本地，见 astore_report_user_event 注释）。 */
        astore_report_user_event("user_add", "admin", 1, det);
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
void pwd_btn_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    dlg_change_pwd();
}

/* role 字符串 → 分段按钮下标。
 * ★ 打开「修改信息」时**必须**用它反查当前角色：s_role_sel 是全局单份状态，
 *   「添加用户」把它初始化成 ROLE_SEL_USER（line 附近 dlg_add_user），编辑弹窗若
 *   沿用这个默认值，会把管理员显示成「普通用户」——用户只是打开看一眼再点保存，
 *   管理员就被降级了（写路径是整记录覆盖）。这是本轮最该防的一条。 */
static role_sel_t role_from_key(const char * role)
{
    for (int i = 0; i < ROLE_SEL_CNT; i++)
        if (strcmp(role, ROLE_KEY[i]) == 0) return (role_sel_t)i;
    return ROLE_SEL_USER;
}

static void dlg_change_pwd(void)
{
    /* 现值一律从 store_cache 快照读（主线程可读、不落盘、不阻塞）。
     * ⚠ 不能用 user_load_all —— 那是同步读盘，违反「认证路径零同步落盘」纪律，
     *   目前只是靠门禁白名单里的历史豁免活着，新代码不该再走老路。 */
    safe_user_t cur;
    memset(&cur, 0, sizeof(cur));
    if (store_cache_find_by_id(s_sel_id, &cur) != 0) {
        /* 快照还没发布（启动后首次加载未回）——此时拿不准当前角色，
         * 与其显示一个可能是错的默认值让人点保存，不如直接不开。 */
        dlg_tip("用户数据尚未加载，请稍候重试");
        return;
    }

    /* 改的是管理员吗？仅当真正改 PIN 时，保存前才强制走动态码二次确认（FR-3） */
    s_pwd_is_admin = (strcmp(cur.role, "admin") == 0);

    s_cur_name[0] = '\0';
    const char * nm = name_cache_get(s_sel_id);          /* 行构建缓存（显示名） */
    if (nm && nm[0])        strncpy(s_cur_name, nm, sizeof(s_cur_name) - 1);
    else if (cur.name[0])   strncpy(s_cur_name, cur.name, sizeof(s_cur_name) - 1);
    s_cur_name[sizeof(s_cur_name) - 1] = '\0';

    strncpy(s_cur_role, cur.role, sizeof(s_cur_role) - 1);
    s_cur_role[sizeof(s_cur_role) - 1] = '\0';
    s_edit_until = cur.valid_until;
    s_edit_limit = cur.use_limit;
    s_edit_used  = cur.used_count;

    s_role_sel = role_from_key(cur.role);      /* ★ 按当前角色反查，不用「添加」的默认值 */

    lv_obj_t * win = dlg_open("修改信息", SX(380), SY(470));  /* 修改信息 */
    (void)win;

    /* 姓名：置顶，预填当前用户名；文字键盘。留空或与原名相同 = 不改名 */
    s_ta_name = dlg_textarea_ex(win, "姓名", false, false, 3, SX(300));
    lv_textarea_set_text(s_ta_name, s_cur_name);

    s_ta_old  = dlg_textarea("旧 PIN",             true, true, 0);
    s_ta_new  = dlg_textarea("新 PIN(留空则只改名)", true, true, 1);  /* 默认焦点（数字键盘） */
    s_ta_new2 = dlg_textarea("确认新 PIN",           true, true, 2);

    /* FR-9 v1.11：角色分段 + 临时授权限制（有效期 / 次数上限）。
     * 与「添加用户」复用同一组控件，区别只在**初值来自当前用户**。 */
    dlg_role_row();
    dlg_temp_row();

    /* 预填现值：次数上限直接填数字；有效期按「剩余天数」折算（向上取整）。
     * 不预填的话，用户改个名字也会把「不限」覆盖上去 —— 那是静默的授权变更。 */
    if (s_ta_limit && s_edit_limit > 0) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", s_edit_limit);
        lv_textarea_set_text(s_ta_limit, buf);
    }
    if (s_ta_days && s_edit_until > 0) {
        int64_t left = s_edit_until - (int64_t)hal_time();
        int days = (int)((left + 86399) / 86400);
        if (days > 0 && days <= 365) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d", days);
            lv_textarea_set_text(s_ta_days, buf);
        }
    }
    /* 已用次数必须让用户看见：改上限要知道基线（上限 ≤ 已用 = 下次认证即删号） */
    if (s_edit_used > 0 && s_msg) {
        char tip[64];
        snprintf(tip, sizeof(tip), "该用户已开锁 %d 次，上限需大于此值", s_edit_used);
        lv_label_set_text(s_msg, tip);
    }

    dlg_sync_role();          /* 刷新选中态 + 按角色显示/隐藏限制行 */
    dlg_btn_row(save_modify_cb);
}

/* 提交（二次确认已通过，或本次不需要确认）。
 * ★ 改名 / 改 PIN / 改角色 / 改限制是**同一次提交**：全部校验在 worker 里
 *   落盘之前完成，要么全改，要么一个字段都不改（详见 astore_modify_user）。 */
static void post_modify_user(const modify_user_req_t * r)
{
    astore_modify_user(r, modify_done);
}

/* 修改结果回填（主线程 worker_poll 派发）。
 * ★ 失败也要 rebuild_list()：早期实现只在成功时刷列表，「改名已落盘、界面还显示
 *   旧名」的不一致就出在这里（部分提交 + 不刷新）。现在失败是零落盘，刷新只是
 *   把界面拉回真实状态，代价极小、收益是永不出现幽灵界面。 */
static void modify_done(int result)
{
    rebuild_list();
    if (result == MODIFY_OK) {
        close_dlg();
        return;
    }
    const char * msg = "保存失败";
    switch (result) {
    case MODIFY_ERR_OLDPIN:     msg = "旧 PIN 错误";            break;
    case MODIFY_ERR_DUPNAME:    msg = "用户名已存在";           break;
    case MODIFY_ERR_LAST_ADMIN: msg = "至少保留一名管理员";     break;
    case MODIFY_ERR_ROLE:       msg = "角色非法";               break;
    case MODIFY_ERR_LIMIT:      msg = "次数上限需大于已用次数"; break;
    default:                                                    break;
    }
    /* 走二次确认的路径上，信息弹窗在确认前就已经关了（s_msg == NULL）——
     * 此时文案要落到独立提示弹窗，否则用户只看到什么都没发生。 */
    if (s_msg) dlg_set_msg(msg);
    else       dlg_tip(msg);
}

static void save_modify_cb(lv_event_t * e)
{
    (void)e;
    if (s_sel_id < 0) return;
    /* 句柄来自上一行 dlg_change_pwd：任一为空说明弹窗被重建过，不猜字段顺序 */
    if (!s_ta_name || !s_ta_old || !s_ta_new || !s_ta_new2) return;

    modify_user_req_t r;
    memset(&r, 0, sizeof(r));
    r.id = s_sel_id;

    const char * name  = lv_textarea_get_text(s_ta_name);
    const char * oldp  = lv_textarea_get_text(s_ta_old);
    const char * newp  = lv_textarea_get_text(s_ta_new);
    const char * newp2 = lv_textarea_get_text(s_ta_new2);

    /* 姓名：非空且与原名不同才改名（留空 = 不改） */
    if (name[0] && strcmp(name, s_cur_name) != 0)
        strncpy(r.new_name, name, sizeof(r.new_name) - 1);

    /* PIN：新 PIN 非空即视为要改密，旧 PIN 为凭据 */
    const bool do_pin = (newp[0] != '\0');
    if (do_pin) {
        const safe_policy_t * pol = user_policy();
        size_t plen = strlen(newp);
        if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
            dlg_set_msg("新 PIN 长度需 4-8 位");
            return;
        }
        if (strcmp(newp, newp2) != 0) {
            dlg_set_msg("两次新 PIN 不一致");
            return;
        }
        strncpy(r.old_pin, oldp, sizeof(r.old_pin) - 1);
        strncpy(r.new_pin, newp, sizeof(r.new_pin) - 1);
    }

    /* 角色：与打开时的角色不同才算改动（不是「与默认不同」） */
    const char * cur_role = ROLE_KEY[s_role_sel];
    const bool do_role = (strcmp(cur_role, s_cur_role) != 0);
    if (do_role) strncpy(r.role, cur_role, sizeof(r.role) - 1);

    /* 临时授权限制：仅当角色为临时用户时读取；与现值不同才算改动。
     * 语义与「添加用户」一致：留空 = 不限。 */
    bool do_limit = false;
    if (strcmp(cur_role, "temp") == 0) {
        const char * ds = s_ta_days  ? lv_textarea_get_text(s_ta_days)  : "";
        const char * ls = s_ta_limit ? lv_textarea_get_text(s_ta_limit) : "";
        int64_t until = 0;
        int     lim   = 0;
        if (ds[0]) {
            int days = atoi(ds);
            if (days <= 0 || days > 365) { dlg_set_msg("有效期需 1-365 天"); return; }
            until = (int64_t)hal_time() + (int64_t)days * 86400;
        }
        if (ls[0]) {
            lim = atoi(ls);
            if (lim <= 0 || lim > 9999) { dlg_set_msg("次数上限需 1-9999 次"); return; }
        }
        r.valid_until = until;
        r.use_limit   = lim;
        if (until != s_edit_until || lim != s_edit_limit) do_limit = true;
    }

    if (!r.new_name[0] && !do_pin && !do_role && !do_limit) {
        dlg_set_msg("姓名、PIN、类别与限制均未改动");
        return;
    }

    /* ★ 敏感操作二次确认（FR-1「角色分配等敏感操作」/ FR-7「在操作处验证」）：
     *   - 改角色 / 改临时授权限制 → 管理员 PIN 二次确认（与删除用户同一套）；
     *   - 管理员改自己 PIN      → 动态码（FR-3，最强）；
     *   两者同时发生时取动态码（它覆盖了管理员 PIN 的强度，且不重复打扰）。 */
    if (s_pwd_is_admin && do_pin) {
        memcpy(&s_pending_mod, &r, sizeof(r));
        close_dlg();          /* 收起信息弹窗 */
        pwd_otp_show();       /* 弹动态码键盘 */
        return;
    }
    if (do_role || do_limit) {
        memcpy(&s_pending_mod, &r, sizeof(r));
        s_auth_for_modify = true;
        close_dlg();
        auth_show_ctx("修改用户类别 / 授权限制需管理员验证");
        return;
    }

    post_modify_user(&r);
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
    if (a->result == 0) astore_report_user_event("user_modify", u.name, 1, u.enabled ? "enabled" : "disabled");
}

static void tog_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        rebuild_list();
    }
    free(a);
}

void toggle_btn_cb(lv_event_t * e)
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
    /* 「修改信息」的待提交请求同样作废：标志不清，下一次删除用户的二次确认
     * 通过后会错误地把上一条修改请求提交出去。 */
    s_auth_for_modify = false;
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
        /* 本次二次确认是为「修改信息」（改角色 / 改临时授权限制）发起的 ——
         * 与上面人脸分叉同一套模式：验完回到该干的那件事。 */
        if (s_auth_for_modify) {
            s_auth_for_modify = false;
            post_modify_user(&s_pending_mod);
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

void del_btn_cb(lv_event_t * e)
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
 *       → 确认弹窗 → hal_face_enroll_async()/delete_async()（立即返回）
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

void face_btn_cb(lv_event_t * e)
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
        r = hal_face_enroll_async(uname);     /* hal_face.h，立即返回 */
    } else if (s_face_op == FACE_OP_DELETE) {
        r = hal_face_delete_async(s_face_pending_tpl);   /* hal_face.h:107 */
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

    if (hal_face_enroll_async(uname) != SAFE_OK) {
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

/* EV_FACE_EVENT 应答：录入成功写回模板号，删除成功写 -1，然后刷新列表 */
void on_face_event_ui(ev_topic_t topic, const void * payload, void * user)
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
    if (a->result == 0) astore_report_user_event("user_del", "admin", 1, name);
}

/* FR-21 防线 2「确认删除结果」：本地用户记录删掉之后，模组侧的人脸模板也必须删掉，
 * 否则会在模组里留下孤儿模板，只能等启动对账兜底 —— 这与「删除即确认模组删除」不符。
 *
 * 为什么不复用「删人脸」对话框那条路径（face_btn_cb → FACE_OP_DELETE →
 * face_start_cb → on_face_event_ui）：那条路径的目标是**用户还在**时解绑模板，
 * 应答回来要写回 face_id=-1 并刷新列表；而这里用户记录已经删掉了，
 * 没有可写回的对象 —— 复用会让 on_face_event_ui 对一个已不存在的 uid 发
 * user_face_set。故此处只复用「hal_face_delete_async + 审计」这一层语义，
 * 不再走 s_face_op 状态机，DELETE_DONE 事件会被 on_face_event_ui 正常忽略
 * （它要求 s_face_op == FACE_OP_DELETE 才处理）。 */
static void del_done(void * p)
{
    user_op_job_t * a = (user_op_job_t *)p;
    if (a->result == 0) {
        if (a->del_face_id >= 0 && (hal_face_caps()->caps & FACE_CAP_DELETE)) {
            safe_err_t r = hal_face_delete_async(a->del_face_id);
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
        if (s_sel_id >= 0 && user_find_by_id(s_sel_id, &du) == 0) {
            a->del_face_id = du.face_id;
            /* 非法 face_id（模组「未分配」占位值或越界）→ 模组侧不可能存在这个模板，
             * 发 DELETE 只会换来一句「容量计数异常」并让用户卡住；直接清本地绑定即可。 */
            if (a->del_face_id == 0xFFFF || a->del_face_id > 0xFFFF) a->del_face_id = -1;
        }
    }
    worker_post(del_worker, a, del_done);
}











/* ================================================================
 *  修改管理员密码 — 动态码二次确认（FR-3 敏感操作）
 *  —— 自绘 6 位动态码键盘，复用 auth_show 的视觉范式；
 *     校验走 auth_admin_verify_totp（任一启用且开 TOTP 的管理员）。
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

/* 动态码二次确认的结果回填（主线程 worker_poll 派发）。
 * ⚠ 结果是**异步**到达的：期间用户可能点了遮罩取消（pwd_otp_close 已把
 *   s_otp_msg 置 NULL 并销毁弹窗）。此时必须直接返回——否则会往一个已经
 *   销毁的 LVGL 对象上写文案。这是所有「异步结果回填 UI」都要先判的一步。 */
static void pwd_otp_verify_done(int result)
{
    if (s_otp_msg == NULL) return;   /* 弹窗已取消 / 销毁：丢弃迟到结果 */

    if (result == 0) {
        pwd_otp_close();
        /* s_pending_mod 在保存时已整包暂存（含角色与限制），动态码通过即原样提交 */
        post_modify_user(&s_pending_mod);
        return;
    }
    s_otp_pin[0] = '\0';
    pwd_otp_update_disp();
    lv_label_set_text(s_otp_msg,
        (result == 2) ? "该动态码已使用，请等下一周期" : "动态码错误，请重试");
}

static void pwd_otp_ok_cb(lv_event_t * e)
{
    (void)e;
    if (strlen(s_otp_pin) != 6) {
        if (s_otp_msg) lv_label_set_text(s_otp_msg, "动态码为 6 位");
        return;
    }
    /* 校验下沉 worker（2026-10-08）：auth_admin_verify_totp 内部是
     * user_load_all 全表读盘 + user_update 整表写盘（回写防重放计数器），
     * 主线程同步调用会卡住 UI 泵并违反技术路线规约 §3.1「认证路径零同步落盘」。
     * 提交后不阻塞，结果由 pwd_otp_verify_done 回填。 */
    if (s_otp_msg) lv_label_set_text(s_otp_msg, "正在验证…");
    astore_admin_verify_totp(s_otp_pin, pwd_otp_verify_done);
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

/* ★ 测试钩子：SAFE_TEST_DLG=change_pwd 直接打开"修改信息"弹窗。
 * s_sel_id 取 1（store_init 建的默认管理员）—— 取 0 是历史写法，但 0 不是真实
 * 用户 id：弹窗改从 store_cache 读现值后会取不到人而拒绝打开，钩子必须给真 id。 */
void page_users_test_open_change_pwd_dlg(void)
{
    s_sel_id = 1;
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


















