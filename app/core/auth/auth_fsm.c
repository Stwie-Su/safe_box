/**
 * @file auth_fsm.c
 * 人脸认证状态机（FR-2 按原因分流，核心实现）。设计要点：
 *
 *  1) 业务层（本文件）只依赖 HAL 接口（hal_face / hal_time / actuator）与存储层，
 *     不直接碰 LVGL / 平台头文件（NFR-5）。
 *  2) 结果分流（需求 v1.6 FR-19，FM225 无分数）：
 *     - OK            → 开锁；
 *     - NO_MATCH      → 计数，连续达 face_otp_after 转 WAIT_OTP，再达 max_failed 锁定；
 *     - LIVENESS_FAIL → 拒绝 + 防伪告警；
 *     - TIMEOUT/ERROR → 不计数（真机连续出帧时的无效帧，不应惩罚用户）。
 *     WAIT_OTP 期间收到 pending 匹配用户的 OK 帧直接放行（人脸本身即凭据，
 *     替代旧「高置信度帧升级」逻辑）；NO_MATCH 继续计数直至锁定。
 *  3) 失败锁定采用「设备级连续失败计数」+「每用户失败计数」双轨：
 *     - max_failed 次连续失败 → 设备级 LOCKOUT（防陌生人轮流试脸遍历攻击）；
 *     - 同时每用户记录 failed_attempts / lock_until（ADR-3，供 PIN 通道单独锁定）。
 *  4) 时间一律走 hal_time()，阶段 5 切 RTC 无需改动此处。
 *
 * 状态超时：
 *   DETECTING face_verify_timeout_s（策略，默认 10s）/ WAIT_OTP 60s /
 *   UNLOCKED 30s / DENY 2s / LOCKOUT lock_seconds。
 */
#include "core/auth/auth_fsm.h"
#include "core/event_bus.h"
#include "core/store/store.h"
#include "core/auth/totp.h"
#include "core/auth/unlock_backend.h"
#include "hal/hal_time.h"
#include "hal/hal_face.h"
#include "hal/hal_actuator.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---------------- 内部状态 ---------------- */
typedef struct {
    fsm_state_t   state;
    face_reason_t last_reason;  /* 最近一次人脸结果（FACE_RES_ERROR = 尚无有效结果） */
    char          pending_user[32];   /* WAIT_OTP 时等待的用户；空串 = 身份未定（NO_MATCH 转入） */
    char          last_user[32];      /* 最近一次开锁/拒绝的用户 */
    uint32_t      ts_enter;           /* 进入当前状态的时刻（hal_time 秒） */
    int           fail_streak;        /* 设备级连续失败计数 */
} fsm_t;

static fsm_t s_fsm;
static auth_ui_hook_t   s_ui_hook   = NULL;
static auth_event_cb_t  s_evt_cb    = NULL;

/* 人脸事件入口：face_service_emit → event_bus(EV_FACE_EVENT) → 本回调（主线程） */
static void handle_face_result(int32_t face_id, face_reason_t reason, const char * mod_name);
static void on_face_event(ev_topic_t topic, const void * payload, void * user);
static void do_unlock(const char * user, const char * via);
static bool bump_fail_streak(const char * user, const char * detail);
static void emit_event(const char * evt, const char * user, const char * detail, int res);
static bool cred_name_ok(const safe_user_t * u, const char * mod_name);

/* 超时（秒）
 *
 * TO_UNLOCKED = 30：开锁后「保险柜已开启」状态保持 30s，期间不重新上锁，
 * 让 UI 来得及显示「已开锁」并允许用户继续操作。
 * 30s 之后自动回到 IDLE（保险柜已上锁）。
 * 实际物理执行器（hal_actuator_pulse）保持 500ms 上限，NFR-7 不变。
 * DETECTING 超时用策略 face_verify_timeout_s（FR-7 系统策略可调，默认 10s）。
 */
#define TO_OTP       60
#define TO_UNLOCKED  30
#define TO_DENY      2
/* LOCKOUT 用策略 lock_seconds（阶段 1 默认 30） */

static void set_state(fsm_state_t st, const char *user, const char *detail)
{
    s_fsm.state = st;
    s_fsm.ts_enter = hal_time();
    if (user) { strncpy(s_fsm.last_user, user, sizeof(s_fsm.last_user) - 1); s_fsm.last_user[sizeof(s_fsm.last_user)-1] = '\0'; }
    if (s_ui_hook) s_ui_hook(st, user ? user : "", detail ? detail : "");
}

void auth_fsm_init(void)
{    memset(&s_fsm, 0, sizeof(s_fsm));
    s_fsm.state = FSM_IDLE;
    s_fsm.last_reason = FACE_RES_ERROR;

    /* 人脸通道（步骤 3a / R6 接线）：订阅总线 EV_FACE_EVENT。
     * 后端结果由 face_service_emit 广播（主线程 publish 同步派发），
     * 后端是模拟器还是 FM225 由构建选项决定，这里不做任何区分。 */
    event_bus_subscribe(EV_FACE_EVENT, on_face_event, NULL);
}

/* 人脸结果分流：核心原则是「绝不重放旧识别」
 *  - IDLE / DETECTING：正常受理；
 *  - WAIT_OTP：pending 匹配用户的 OK 帧直接放行（人脸本身即凭据）；
 *    NO_MATCH 继续计数（达 max_failed 锁定）；LIVENESS_FAIL 拒绝并告警；
 *    TIMEOUT/ERROR 丢弃。真机连续出帧，若一律丢弃，用户刷脸成功也要干等超时；
 *  - UNLOCKED / DENY / LOCKOUT 等暂态：直接丢弃，否则开锁动画期间
 *    检测到的人脸会在回到 IDLE 后被重放误触发。 */
static void handle_face_result(int32_t face_id, face_reason_t reason, const char * mod_name)
{
    if(s_fsm.state == FSM_IDLE || s_fsm.state == FSM_DETECTING) {
        auth_fsm_submit_face_ex(face_id, reason, mod_name);
    }
    else if(s_fsm.state == FSM_WAIT_OTP) {
        if(reason == FACE_RES_OK) {
            safe_user_t u;
            if(face_id >= 0 && user_find_by_face(face_id, &u) == 0) {
                /* FR-21 防线 1 同样适用于「等待动态码期间的刷脸放行」这条路 */
                if(!cred_name_ok(&u, mod_name)) {
                    s_fsm.pending_user[0] = '\0';
                    set_state(FSM_DENY, u.name, "人脸凭据不一致");
                    return;
                }
                s_fsm.pending_user[0] = '\0';
                do_unlock(u.name, "face");
            }
        }
        else if(reason == FACE_RES_NO_MATCH) {
            /* 已在等待动态码仍刷不出来：继续计数，达阈值进 LOCKOUT */
            bump_fail_streak(NULL, "人脸未匹配");
            /* 未达锁定阈值：留在 WAIT_OTP 继续等动态码 */
        }
        else if(reason == FACE_RES_LIVENESS_FAIL) {
            /* 防伪告警（FR-10）：拒绝并退出等待 */
            log_append("ALARM", "-", 0, "活体检测失败（防伪）");
            emit_event("ALARM", "-", "liveness fail", 0);
            if(!bump_fail_streak(NULL, "活体检测失败"))
                set_state(FSM_DENY, NULL, "活体检测失败");
        }
        /* TIMEOUT / ERROR：不计数不迁移 */
    }
    /* 其余状态：丢弃 */
}

/* 人脸事件回调（总线签名）：payload 为 ev_face_event_t（POD，步骤 3a），
 * 由 face_service_emit → event_bus_publish 在主线程同步派发。 */
static void on_face_event(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if(topic != EV_FACE_EVENT || payload == NULL) return;
    const ev_face_event_t * e = (const ev_face_event_t *)payload;
    if(e->ev != FACE_EV_DETECT) return;   /* ENROLL/DELETE/ERROR 与认证状态机无关 */
    /* mod_name：模组返回的模板名，供 FR-21 核对；空串表示模组未提供 */
    handle_face_result(e->res.face_id, e->res.reason, e->res.user_name);
}

void auth_fsm_set_ui_hook(auth_ui_hook_t hook) { s_ui_hook = hook; }
void auth_fsm_set_event_cb(auth_event_cb_t cb) { s_evt_cb = cb; }

static void emit_event(const char *evt, const char *user, const char *detail, int res)
{
    if (s_evt_cb) s_evt_cb(evt, user ? user : "", detail ? detail : "", res);
}

/* 设备级失败计数 + 锁定判定；user 为失败归属（可为 NULL）。
 * 返回 true 表示已达 max_failed 并进入 LOCKOUT（调用方不应再改状态）。 */
static bool bump_fail_streak(const char *user, const char *detail)
{
    const safe_policy_t *p = user_policy();
    s_fsm.fail_streak++;

    /* 每用户计数（用于 PIN 通道单独锁定 / 审计） */
    if (user && *user) {
        safe_user_t u;
        if (user_find_by_name(user, &u) == 0) {
            u.failed_attempts++;
            if (u.failed_attempts >= p->max_failed) {
                u.lock_until = (int64_t)hal_time() + p->lock_seconds;
                u.failed_attempts = 0;
            }
            user_update(&u);
        }
    }

    log_append("DENY", user ? user : "-", 0, detail ? detail : "");
    emit_event("DENY", user, detail, 0);

    if (s_fsm.fail_streak >= p->max_failed) {
        s_fsm.fail_streak = 0;
        char buf[48];
        snprintf(buf, sizeof(buf), "连续 %d 次失败，锁定 %ds", p->max_failed, p->lock_seconds);
        log_append("LOCKOUT", "-", 0, buf);
        emit_event("LOCKOUT", "-", buf, 0);
        set_state(FSM_LOCKOUT, NULL, buf);
        return true;
    }
    return false;
}

static void note_fail(const char *user, const char *detail)
{
    if (!bump_fail_streak(user, detail))
        set_state(FSM_DENY, user, detail);
}

static void do_unlock(const char *user, const char *via)
{
    safe_user_t u;
    if (user_find_by_name(user, &u) != 0) { note_fail(user, "用户不存在"); return; }

    hal_actuator_pulse(500);   /* NFR-7：硬性 500ms 上限，截断在实现里 */

    char detail[64];
    snprintf(detail, sizeof(detail), "via=%s", via ? via : "?");
    log_append("UNLOCK", user, 1, detail);
    emit_event("UNLOCK", user, detail, 1);

    /* 临时用户次数计数；用完即删（FR-9 v1.6：直接删除用户，保留审计日志） */
    if (strcmp(u.role, "temp") == 0 && u.use_limit > 0) {
        u.used_count++;
        if (u.used_count >= u.use_limit) {
            user_del(u.id);
            log_append("ALARM", user, 1, "临时授权次数已用尽，用户已删除");
            emit_event("ALARM", user, "temp used up, deleted", 1);
        } else {
            u.failed_attempts = 0;
            u.lock_until = 0;
            user_update(&u);
        }
    } else {
        u.failed_attempts = 0;
        u.lock_until = 0;
        user_update(&u);
    }

    s_fsm.fail_streak = 0;
    set_state(FSM_UNLOCKED, user, detail);
}

/* 人脸未匹配：计数并按阈值决定转动态码或拒绝 */
static void face_no_match(safe_user_t *user)
{
    const safe_policy_t *p = user_policy();
    if (bump_fail_streak(user ? user->name : NULL, "人脸未匹配"))
        return;   /* 已进 LOCKOUT */

    if (s_fsm.fail_streak >= p->face_otp_after) {
        /* 连续未匹配达阈值：强制转动态码（FR-2 按原因分流）。
         * pending_user 留空表示身份未定（模组未给出匹配 ID），
         * 动态码按「任一启用 TOTP 的用户」校验（见 submit_otp）。
         * 若 face_otp_after >= max_failed，会先触发锁定，本分支不可达——配置时注意。 */
        s_fsm.pending_user[0] = '\0';
        set_state(FSM_WAIT_OTP, NULL, "请使用动态密码二次确认");
    } else {
        set_state(FSM_DENY, user ? user->name : NULL, "人脸未匹配");
    }
}

/* 活体失败：拒绝 + 防伪告警（FR-19 / FR-10） */
static void liveness_fail(safe_user_t *user)
{
    log_append("ALARM", user ? user->name : "-", 0, "活体检测失败（防伪）");
    emit_event("ALARM", user ? user->name : "-", "liveness fail", 0);
    if (!bump_fail_streak(user ? user->name : NULL, "活体检测失败"))
        set_state(FSM_DENY, user ? user->name : NULL, "活体检测失败");
}

/* FR-21 防线 1（v1.4）：核对模组返回的模板名与本地区名是否一致。
 * 返回 false = 凭据不一致：已记审计与告警、且已把本地人脸凭据标记失效，调用方必须拒绝开锁。
 * 不一致的含义不是「用户敲错了」，而是「模组侧这张脸绑的不是本地记录的这个人」
 * —— 换绑、冒用、或模组被整体替换过，属凭据失效而非认证失败，故不并入失败计数
 * （不是暴力尝试），但同样不放过。PIN / 动态码通道不受影响。 */
static bool cred_name_ok(const safe_user_t * u, const char * mod_name)
{
    if(u == NULL) return false;
    if(mod_name == NULL || *mod_name == '\0') return true;   /* 模组未提供名字：跳过核对 */
    if(strcmp(mod_name, u->name) == 0) return true;

    char detail[64];
    snprintf(detail, sizeof(detail), "模组名[%s]≠本地名[%s]", mod_name, u->name);
    log_append("ALARM", u->name, 0, detail);
    emit_event("ALARM", u->name, "credential mismatch", 0);

    safe_user_t upd;
    if(user_find_by_name(u->name, &upd) == 0 && upd.face_enable) {
        upd.face_enable = false;            /* 本地人脸凭据标记失效（FR-21） */
        user_update(&upd);
        log_append("ALARM", u->name, 0, "人脸凭据已标记失效，请管理员重新录入");
    }
    return false;
}

/* 解析一次人脸结果：user 为 NULL 表示模组未给出匹配身份 */
static void resolve(int32_t face_id, face_reason_t reason, safe_user_t *user,
                    const char * mod_name)
{
    (void)face_id;   /* 命中用户已由 user_find_by_face 解析，此处仅保留接口以备审计 */

    if (!user) {
        /* 模组报 OK 却找不到用户：数据不一致，按陌生人处理 */
        if (reason == FACE_RES_OK) { note_fail(NULL, "未注册人脸"); return; }
        if (reason == FACE_RES_NO_MATCH)    { face_no_match(NULL);    return; }
        if (reason == FACE_RES_LIVENESS_FAIL){ liveness_fail(NULL);   return; }
        return;   /* TIMEOUT/ERROR 不计数 */
    }
    if (!user->enabled) {
        note_fail(user->name, "用户已停用");
        return;
    }
    if (!user->face_enable) {
        note_fail(user->name, "人脸通道未启用");
        return;
    }
    /* FR-9（需求 v1.6）：临时用户到期 / 次数耗尽 → 直接删除该用户（含人脸凭据），
     * 删除动作记审计日志。（v1.5 及以前为「保留记录，置 disabled」，已按 v1.6 收紧。） */
    if (strcmp(user->role, "temp") == 0) {
        const uint32_t now = hal_time();
        bool expired = (user->valid_until > 0 && (int64_t)now >= user->valid_until);
        bool used_up = (user->use_limit > 0 && user->used_count >= user->use_limit);
        if (expired || used_up) {
            char why[64];
            snprintf(why, sizeof(why), "临时授权%s，用户已删除",
                     expired ? "已过期" : "次数已用尽");
            user_del(user->id);
            log_append("ALARM", user->name, 1, why);
            emit_event("ALARM", user->name, why, 1);
            return;   /* 授权失效不是暴力尝试，不计设备级失败 */
        }
    }

    /* FR-21 防线 1：模组说「是这个人」，还要核对它报的名字与我们记录的是否一致。
     * 不一致时 cred_name_ok 内部已记审计与告警、并把本地人脸凭据标记失效；
     * 这里只负责拒绝开锁 —— 它不是「未匹配」（不是暴力尝试），故不计入失败计数。 */
    if(reason == FACE_RES_OK && !cred_name_ok(user, mod_name)) {
        set_state(FSM_DENY, user->name, "人脸凭据不一致");
        return;
    }

    switch (reason) {
        case FACE_RES_OK:            do_unlock(user->name, "face"); break;
        case FACE_RES_NO_MATCH:      face_no_match(user);           break;
        case FACE_RES_LIVENESS_FAIL: liveness_fail(user);           break;
        default:                     break;   /* TIMEOUT/ERROR 不计数不迁移 */
    }
}

void auth_fsm_submit_face(int32_t face_id, face_reason_t reason)
{
    auth_fsm_submit_face_ex(face_id, reason, NULL);
}

void auth_fsm_submit_face_ex(int32_t face_id, face_reason_t reason, const char * mod_name)
{
    if (s_fsm.state == FSM_LOCKOUT) return;
    if (reason == FACE_RES_TIMEOUT || reason == FACE_RES_ERROR) return;   /* 不计数不迁移 */

    s_fsm.last_reason = reason;
    safe_user_t u;
    safe_user_t *pu = (face_id >= 0 && user_find_by_face(face_id, &u) == 0) ? &u : NULL;
    /* mod_name 直接穿透给 resolve（本函数在主线程被 event_bus_pump 调用，无并发问题） */
    resolve(face_id, reason, pu, mod_name);
}

void auth_fsm_submit_pin(const char *pin)
{
    if (s_fsm.state == FSM_LOCKOUT) return;
    char user[32] = {0};
    int r = backend_verify_pin(pin, user, sizeof(user));
    if (r == UNLOCK_OK) {
        s_fsm.last_reason = FACE_RES_ERROR;   /* 本次开锁与人脸无关 */
        do_unlock(user, "pin");
    } else if (r == UNLOCK_LOCKED) {
        note_fail(NULL, "设备已锁定");
    } else {
        note_fail(user[0] ? user : NULL, "PIN 错误");
    }
}

void auth_fsm_submit_otp(const char *code)
{
    if (s_fsm.state != FSM_WAIT_OTP) return;

    /* NO_MATCH 连续转入时身份未定（pending_user 为空）：
     * 动态码按「任一启用 TOTP 的用户」校验，通过者开锁。 */
    if (!s_fsm.pending_user[0]) {
        char name[32] = {0};
        auth_result_t r0 = backend_verify_totp_any(code, name, sizeof(name));
        if (r0 == AUTH_OK) {
            do_unlock(name, "otp");
        } else if (r0 == AUTH_REPLAY) {
            note_fail(NULL, "动态码已使用");
        } else {
            note_fail(NULL, "动态码错误");   /* 与指定用户路径一致：错码退出 WAIT_OTP → DENY */
        }
        return;
    }

    auth_result_t r = backend_verify_totp(s_fsm.pending_user, code);
    char name[32];
    strncpy(name, s_fsm.pending_user, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    s_fsm.pending_user[0] = '\0';

    if (r == AUTH_OK) {
        s_fsm.last_reason = FACE_RES_ERROR;
        do_unlock(name, "otp");
    } else if (r == AUTH_REPLAY) {
        note_fail(name, "动态码已使用");
    } else if (r == AUTH_LOCKED) {
        note_fail(name, "用户已锁定");
    } else {
        note_fail(name, "动态码错误");
    }
}

void auth_fsm_cancel_otp(void)
{
    if (s_fsm.state != FSM_WAIT_OTP) return;
    s_fsm.pending_user[0] = '\0';
    set_state(FSM_IDLE, NULL, "已取消动态码验证");
}

void auth_fsm_note_unlock(const char *user)
{
    if (!user) return;
    s_fsm.last_reason = FACE_RES_ERROR;
    do_unlock(user, "remote");
}

void auth_fsm_note_failure(const char *detail)
{
    note_fail(NULL, detail ? detail : "验证失败");
}

/* ---------------- 主线程 tick ---------------- */
void auth_fsm_tick(void)
{
    const uint32_t now = hal_time();

    /* 识别结果不再在这里轮询，改由总线 EV_FACE_EVENT 事件驱动（见 on_face_event）。
     * tick 只负责状态超时推进。 */
    uint32_t elapsed = now - s_fsm.ts_enter;
    switch (s_fsm.state) {
        case FSM_DETECTING:
            /* 人脸验证过程超时（FR-7 可调，默认 10s）：无人脸/无结果回 IDLE */
            if (elapsed >= (uint32_t)user_policy()->face_verify_timeout_s)
                set_state(FSM_IDLE, NULL, "识别超时");
            break;
        case FSM_WAIT_OTP:
            if (elapsed >= TO_OTP) {
                s_fsm.pending_user[0] = '\0';
                note_fail(NULL, "动态码超时");
            }
            break;
        case FSM_UNLOCKED:
            if (elapsed >= TO_UNLOCKED) set_state(FSM_IDLE, NULL, "");
            break;
        case FSM_DENY:
            if (elapsed >= TO_DENY) set_state(FSM_IDLE, NULL, "");
            break;
        case FSM_LOCKOUT:
            if (elapsed >= (uint32_t)user_policy()->lock_seconds) {
                set_state(FSM_IDLE, NULL, "");
            }
            break;
        default:
            break;
    }
}

/* ---------------- 查询接口 ---------------- */
fsm_state_t auth_fsm_state(void) { return s_fsm.state; }
const char * auth_fsm_state_name(fsm_state_t s)
{
    switch (s) {
        case FSM_IDLE:      return "IDLE";
        case FSM_DETECTING: return "DETECTING";
        case FSM_UNLOCKED:  return "UNLOCKED";
        case FSM_WAIT_OTP:  return "WAIT_OTP";
        case FSM_DENY:      return "DENY";
        case FSM_LOCKOUT:   return "LOCKOUT";
        default:            return "?";
    }
}
face_reason_t auth_fsm_last_reason(void) { return s_fsm.last_reason; }
const char * auth_fsm_pending_user(void) { return s_fsm.pending_user; }
int auth_fsm_lock_remaining(void)
{
    if (s_fsm.state != FSM_LOCKOUT) return 0;
    int left = (int)(user_policy()->lock_seconds - (hal_time() - s_fsm.ts_enter));
    return left < 0 ? 0 : left;
}

/* 设备级连续失败计数（只读，规约 §5.2 / UI 现代化 ui1）。
 * UI 只在本接口上展示「未匹配（n/face_otp_after）」，不得据此改状态。 */
int auth_fsm_fail_streak(void)
{
    return s_fsm.fail_streak;
}

