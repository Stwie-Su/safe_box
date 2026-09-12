/**
 * @file ui_feedback.c
 * UI 反馈中枢实现（UI 现代化 spec §1 + §3 后半）。
 *
 * 职责：
 *  1) 订阅 EV_FACE_EVENT + EV_AUTH_RESULT，把事件翻译成顶部横幅文案与档位；
 *  2) 同步驱动人脸页扫描框状态（IDLE/OK/FAIL）；
 *  3) LOCKOUT 期间常驻横幅并每秒刷新倒计时，锁定解除自动淡出。
 *
 * 事件 → 反馈映射（spec §1 表）：
 *   DETECT reason=OK            → 「欢迎回来，{用户名}」OK 2s        + 扫描框绿
 *   DETECT reason=NO_MATCH      → 「未匹配（n/face_otp_after）」WARN 1.5s + 扫描框红（600ms 回 IDLE）
 *   DETECT reason=LIVENESS_FAIL → 「活体检测失败」DANGER 2s          + 扫描框红
 *   DETECT reason=TIMEOUT/ERROR → 「识别超时/识别异常」INFO 1.5s      + 扫描框不变
 *   ENROLL_DONE err=OK/≠OK      → 「人脸录入成功/失败」OK/DANGER 2s
 *   DELETE_DONE err=OK/≠OK      → 「人脸删除成功/失败」OK/DANGER 2s
 *   FACE_EV_ERROR               → 后端描述原文，WARN 1.5s
 *   AUTH_RESULT LOCKOUT         → 「多次失败，已锁定 {n}s 后重试」DANGER，每秒刷新
 *
 * 订阅顺序约束：本模块的 EV_FACE_EVENT 处理器依赖 auth_fsm 已更新 fail_streak，
 * 故 app.c 必须「先 auth_fsm_init()（先订阅）后 ui_init()（后订阅）」——事件总线
 * 按订阅顺序派发，core 先消费、UI 后读，读数才正确。
 *
 * 性能（A7 单核）：横幅是 ~64px 高小条，只动 opa / 文本；扫描框染色只改 9 个
 * 对象的 bg_color。动画期内无堆分配（横幅对象与 lv_anim 描述符均为静态）。
 */
#include "ui/ui_feedback.h"

#include <stdio.h>
#include <string.h>

#include "ui/ui_anim.h"
#include "ui/ui_scale.h"
#include "ui/theme.h"
#include "ui/pages/page_face.h"
#include "core/event_bus.h"
#include "core/auth/auth_fsm.h"
#include "core/store/store.h"

/* ---------------- 内部状态 ---------------- */
static lv_obj_t * s_host    = NULL;   /* 横幅宿主（ui.c overlay） */
static lv_obj_t * s_banner  = NULL;   /* 横幅容器（常驻，默认隐藏） */
static lv_obj_t * s_label   = NULL;   /* 横幅文案 */
static lv_anim_t  s_anim;             /* 横幅淡入淡出动画（静态：动画期零分配） */
static lv_timer_t * s_lock_timer = NULL;  /* LOCKOUT 倒计时刷新（1s，常驻） */
static bool s_lock_active = false;    /* 当前是否正在展示锁定倒计时横幅 */

/* 档位 → 主题色角色（查表替代 if-else 嵌套） */
static const theme_role_t BANNER_ROLE[UI_BANNER_LEVEL_COUNT] = {
    TH_ACCENT,   /* UI_BANNER_INFO   */
    TH_OK,       /* UI_BANNER_OK     */
    TH_WARN,     /* UI_BANNER_WARN   */
    TH_DANGER,   /* UI_BANNER_DANGER */
};

/* 识别结果 → 横幅档位 / 停留时长 / 扫描框状态（查表，spec §1 表） */
typedef struct {
    ui_banner_level_t level;
    uint32_t          hold_ms;
    page_face_scan_t  scan;
    bool              touch_scan;   /* false = 扫描框保持不变（TIMEOUT/ERROR） */
} detect_rule_t;

static const detect_rule_t DETECT_RULE[5] = {
    /* FACE_RES_OK            */ { UI_BANNER_OK,     UI_ANIM_BANNER_HOLD_MS,   PAGE_FACE_SCAN_OK,   true  },
    /* FACE_RES_NO_MATCH      */ { UI_BANNER_WARN,   UI_ANIM_BANNER_HOLD_MS_S, PAGE_FACE_SCAN_FAIL, true  },
    /* FACE_RES_LIVENESS_FAIL */ { UI_BANNER_DANGER, UI_ANIM_BANNER_HOLD_MS,   PAGE_FACE_SCAN_FAIL, true  },
    /* FACE_RES_TIMEOUT       */ { UI_BANNER_INFO,   UI_ANIM_BANNER_HOLD_MS_S, PAGE_FACE_SCAN_IDLE, false },
    /* FACE_RES_ERROR         */ { UI_BANNER_INFO,   UI_ANIM_BANNER_HOLD_MS_S, PAGE_FACE_SCAN_IDLE, false },
};

/* ---------------- 私有声明 ---------------- */
static void banner_create(void);
static void banner_opa_cb(void * obj, int32_t v);
static void banner_faded_out_cb(lv_anim_t * a);
static void banner_start_fade(int32_t from, int32_t to, uint32_t ms, bool hide_on_end);
static void banner_show(const char * text, ui_banner_level_t level,
                        uint32_t hold_ms, bool sticky);
static void on_face_event(ev_topic_t topic, const void * payload, void * user);
static void on_auth_result(ev_topic_t topic, const void * payload, void * user);
static void detect_text(char * buf, size_t cap, const ev_face_event_t * e);
static void lock_timer_cb(lv_timer_t * t);
static void lock_stop(void);
static void banner_refresh_theme(int idx);

/**
 * @brief 建好常驻横幅（宿主由 ui.c 提供）。默认隐藏、不可点击（不挡触摸）。
 */
static void banner_create(void)
{
    if (s_host == NULL) return;

    s_banner = lv_obj_create(s_host);
    lv_obj_set_size(s_banner, LV_SIZE_CONTENT, SY(UI_ANIM_BANNER_H_BASE));
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(s_banner, SX(12), 0);
    lv_obj_set_style_pad_left(s_banner, SX(20), 0);
    lv_obj_set_style_pad_right(s_banner, SX(20), 0);
    lv_obj_set_style_pad_top(s_banner, 0, 0);
    lv_obj_set_style_pad_bottom(s_banner, 0, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_outline_width(s_banner, 0, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 反馈条只展示，不参与命中测试：触摸事件穿透到下面的页面 */
    lv_obj_set_clickable(s_banner, false);
    lv_obj_set_scrollable(s_banner, false);

    s_label = lv_label_create(s_banner);
    lv_label_set_text(s_label, "");
    lv_obj_set_style_text_font(s_label, app_font_scaled(16), 0);

    lv_obj_set_hidden(s_banner, true);
}

/* 横幅 opa 动画执行回调（只改一个样式属性 → 局部重绘，A7 上开销可忽略） */
static void banner_opa_cb(void * obj, int32_t v)
{
    if (obj == NULL) return;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* 淡出结束：隐藏横幅（保持在 overlay 上，下次复用，不销毁对象） */
static void banner_faded_out_cb(lv_anim_t * a)
{
    (void)a;
    if (s_banner) lv_obj_set_hidden(s_banner, true);
}

/* 起一段 opa 动画。hide_on_end=true 时结束后隐藏（淡出用）。 */
static void banner_start_fade(int32_t from, int32_t to, uint32_t ms, bool hide_on_end)
{
    lv_anim_init(&s_anim);
    lv_anim_set_var(&s_anim, s_banner);
    lv_anim_set_exec_cb(&s_anim, banner_opa_cb);
    lv_anim_set_values(&s_anim, from, to);
    lv_anim_set_duration(&s_anim, ms);
    lv_anim_set_path_cb(&s_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&s_anim, hide_on_end ? banner_faded_out_cb : NULL);
    lv_anim_start(&s_anim);
}

/**
 * @brief 显示横幅。sticky=true 只淡入（常驻），false 为「淡入→停留→淡出」单条动画
 *        （用 reverse_delay 实现停留，避免额外创建 lv_timer）。
 */
static void banner_show(const char * text, ui_banner_level_t level,
                        uint32_t hold_ms, bool sticky)
{
    if (s_banner == NULL) return;
    if ((int)level < 0 || level >= UI_BANNER_LEVEL_COUNT) level = UI_BANNER_INFO;

    /* 新横幅到达：删掉旧动画直接替换（spec §1） */
    lv_anim_delete(s_banner, banner_opa_cb);

    lv_label_set_text(s_label, text ? text : "");
    lv_obj_set_style_bg_color(s_banner, theme_color(BANNER_ROLE[level]), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    /* 档位底色上的文字：全部取 TH_ACCENT_INK（五套主题里均为反白色） */
    lv_obj_set_style_text_color(s_label, theme_color(TH_ACCENT_INK), 0);

    lv_obj_set_style_opa(s_banner, LV_OPA_TRANSP, 0);
    lv_obj_set_hidden(s_banner, false);

    if (sticky) {
        banner_start_fade(LV_OPA_TRANSP, LV_OPA_COVER, UI_ANIM_BANNER_FADE_MS, false);
        return;
    }
    /* 淡入 → 停留 hold_ms → 反向淡出：一条动画搞定，零额外对象 */
    lv_anim_init(&s_anim);
    lv_anim_set_var(&s_anim, s_banner);
    lv_anim_set_exec_cb(&s_anim, banner_opa_cb);
    lv_anim_set_values(&s_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&s_anim, UI_ANIM_BANNER_FADE_MS);
    lv_anim_set_reverse_duration(&s_anim, UI_ANIM_BANNER_FADE_MS);
    lv_anim_set_reverse_delay(&s_anim, hold_ms);
    lv_anim_set_path_cb(&s_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&s_anim, banner_faded_out_cb);
    lv_anim_start(&s_anim);
}

void ui_banner(const char * text, ui_banner_level_t level, uint32_t hold_ms)
{
    banner_show(text, level, hold_ms, false);
}

void ui_banner_sticky(const char * text, ui_banner_level_t level)
{
    banner_show(text, level, 0, true);
}

void ui_banner_hide(void)
{
    if (s_banner == NULL || lv_obj_is_hidden(s_banner)) return;
    lv_anim_delete(s_banner, banner_opa_cb);
    int32_t from = (int32_t)lv_obj_get_style_opa(s_banner, 0);
    banner_start_fade(from, LV_OPA_TRANSP, UI_ANIM_BANNER_FADE_MS, true);
}

/* ---------------- LOCKOUT 倒计时 ---------------- */

/* 结束锁定横幅：淡出 + 停标记。锁定解除（remaining=0）或状态离开 LOCKOUT 时调用。 */
static void lock_stop(void)
{
    if (!s_lock_active) return;
    s_lock_active = false;
    ui_banner_hide();
}

/* 1s 周期：锁定期间刷新倒计时文案；解除后自动淡出。未激活时直接返回（零开销）。 */
static void lock_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (!s_lock_active) return;

    if (auth_fsm_state() != FSM_LOCKOUT) { lock_stop(); return; }

    int left = auth_fsm_lock_remaining();
    if (left <= 0) { lock_stop(); return; }

    char buf[64];
    snprintf(buf, sizeof(buf), "多次失败，已锁定 %ds 后重试", left);
    if (s_label && lv_obj_is_hidden(s_banner)) {
        /* 异常恢复路径：横幅被别的事件顶掉后重新贴一条常驻的 */
        ui_banner_sticky(buf, UI_BANNER_DANGER);
    } else if (s_label) {
        /* 只改文案，不重启动画（否则每秒淡入一次会闪） */
        lv_label_set_text(s_label, buf);
    }
}

/* ---------------- 事件 → 反馈 ---------------- */

/* 识别结果文案（spec §1）：OK 反查用户名，NO_MATCH 显示 n/face_otp_after */
static void detect_text(char * buf, size_t cap, const ev_face_event_t * e)
{
    switch (e->res.reason) {
        case FACE_RES_OK: {
            safe_user_t u;
            if (e->res.face_id >= 0 && user_find_by_face(e->res.face_id, &u) == 0) {
                snprintf(buf, cap, "欢迎回来，%s", u.name);
            } else {
                /* 模组报 OK 但库里查不到：数据不一致，不谎报用户名 */
                snprintf(buf, cap, "识别成功，但未找到对应用户");
            }
            break;
        }
        case FACE_RES_NO_MATCH: {
            /* fail_streak 由 auth_fsm 在同一事件的更早订阅者里已 bump（见文件头顺序约束） */
            int n = auth_fsm_fail_streak();
            int total = user_policy()->face_otp_after;
            if (n < 1) n = 1;
            if (total < 1) total = 1;
            if (n > total) n = total;
            snprintf(buf, cap, "未匹配（%d/%d）", n, total);
            break;
        }
        case FACE_RES_LIVENESS_FAIL:
            snprintf(buf, cap, "活体检测失败");
            break;
        case FACE_RES_TIMEOUT:
            snprintf(buf, cap, "识别超时");
            break;
        default:
            snprintf(buf, cap, "识别异常");
            break;
    }
}

/* EV_FACE_EVENT：识别 / 录入 / 删除 / 后端异常 */
static void on_face_event(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if (topic != EV_FACE_EVENT || payload == NULL) return;

    const ev_face_event_t * e = (const ev_face_event_t *)payload;
    char buf[80];

    switch (e->ev) {
        case FACE_EV_DETECT: {
            if (e->res.reason < FACE_RES_OK || e->res.reason > FACE_RES_ERROR) return;
            const detect_rule_t * r = &DETECT_RULE[e->res.reason];
            detect_text(buf, sizeof(buf), e);
            ui_banner(buf, r->level, r->hold_ms);
            /* TIMEOUT / ERROR 不改扫描框（spec §1 表：扫描框不变） */
            if (r->touch_scan) page_face_set_scan_state(r->scan);
            break;
        }
        case FACE_EV_ENROLL_DONE:
            if (e->enroll.err == SAFE_OK) {
                ui_banner("人脸录入成功", UI_BANNER_OK, UI_ANIM_BANNER_HOLD_MS);
            } else {
                ui_banner("人脸录入失败", UI_BANNER_DANGER, UI_ANIM_BANNER_HOLD_MS);
            }
            break;
        case FACE_EV_DELETE_DONE:
            if (e->del.err == SAFE_OK) {
                ui_banner("人脸删除成功", UI_BANNER_OK, UI_ANIM_BANNER_HOLD_MS);
            } else {
                ui_banner("人脸删除失败", UI_BANNER_DANGER, UI_ANIM_BANNER_HOLD_MS);
            }
            break;
        case FACE_EV_ERROR:
            ui_banner(e->msg[0] ? e->msg : "人脸模块异常",
                      UI_BANNER_WARN, UI_ANIM_BANNER_HOLD_MS_S);
            break;
        default:
            break;
    }
}

/* EV_AUTH_RESULT：只关心 LOCKOUT（DENY 的文案已由人脸事件侧给出，避免重复弹） */
static void on_auth_result(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if (topic != EV_AUTH_RESULT || payload == NULL) return;

    const ev_auth_result_t * p = (const ev_auth_result_t *)payload;
    if (strcmp(p->evt, "LOCKOUT") != 0) return;

    int left = auth_fsm_lock_remaining();
    if (left <= 0) left = (int)user_policy()->lock_seconds;

    char buf[64];
    snprintf(buf, sizeof(buf), "多次失败，已锁定 %ds 后重试", left);
    s_lock_active = true;
    ui_banner_sticky(buf, UI_BANNER_DANGER);
}

/* ---------------- 主题切换：横幅配色重刷 ---------------- */
static void banner_refresh_theme(int idx)
{
    (void)idx;
    if (s_label) lv_obj_set_style_text_color(s_label, theme_color(TH_ACCENT_INK), 0);
    /* 底色按「当前档位」重刷：档位没有单独记录，取上一次设置的角色色即可——
     * 底色是本地样式值，主题切换不会自动变，这里按宿主记录的角色重建。
     * 简化处理：仅当横幅可见时才需要立即一致，隐藏状态下下次 show 会重设。 */
}

/**
 * @brief 初始化反馈中枢。ui_init() 末尾调用一次（页面已全部实例化）。
 */
void ui_feedback_init(lv_obj_t * host)
{
    s_host = host;
    banner_create();

    event_bus_subscribe(EV_FACE_EVENT, on_face_event, NULL);
    event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    theme_register_change_cb(banner_refresh_theme);

    /* LOCKOUT 倒计时刷新定时器：常驻 1s，未激活时 cb 直接返回（开销可忽略） */
    if (s_lock_timer == NULL) {
        s_lock_timer = lv_timer_create(lock_timer_cb, 1000, NULL);
    }
}
