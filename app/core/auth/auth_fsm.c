/**
 * @file auth_fsm.c
 * 置信度分级状态机（FR-2 核心实现）。设计要点：
 *
 *  1) 业务层（本文件）只依赖 HAL 接口（hal_face / hal_time / actuator）与存储层，
 *     不直接碰 LVGL / 平台头文件（NFR-5）。
 *  2) 失败锁定采用「设备级连续失败计数」+「每用户失败计数」双轨：
 *     - max_failed 次连续失败 → 设备级 LOCKOUT 30s（防陌生人轮流试脸遍历攻击）；
 *     - 同时每用户记录 failed_attempts / lock_until（ADR-3，供 PIN 通道单独锁定）。
 *     这是对 ADR-3「不触发全局锁定」的务实修正：保险柜设备维度必须有全局锁定，
 *     否则攻击者可用不同人脸轮流试错；详见工程笔记阶段一记录。
 *  3) 时间一律走 hal_time()，阶段 5 切 RTC 无需改动此处。
 *
 * 状态超时：
 *   DETECTING 5s（无结果回 IDLE） / WAIT_OTP 60s / UNLOCKED 3s / DENY 2s / LOCKOUT 30s。
 */
#include "core/auth/auth_fsm.h"
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
    fsm_state_t state;
    int        last_score;
    char       pending_user[32];   /* WAIT_OTP 时等待的用户 */
    char       last_user[32];      /* 最近一次开锁/拒绝的用户 */
    uint32_t   ts_enter;           /* 进入当前状态的时刻（hal_time 秒） */
    int        fail_streak;        /* 设备级连续失败计数 */
} fsm_t;

static fsm_t s_fsm;
static auth_ui_hook_t   s_ui_hook   = NULL;
static auth_event_cb_t  s_evt_cb    = NULL;

/* 人脸事件入口：由 face_service 在主线程回调，等价于旧的 tick 轮询 */
static void handle_detect_frame(int face_id, int score);
static void on_face_event(face_event_t ev, const void * payload, void * user);
static void do_unlock(const char * user);

/* 超时（秒） */
#define TO_DETECTING 5
#define TO_OTP       60
#define TO_UNLOCKED  3
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
{
    memset(&s_fsm, 0, sizeof(s_fsm));
    s_fsm.state = FSM_IDLE;
    s_fsm.last_score = -1;

    /* 人脸通道：初始化后端并订阅识别结果。后端是模拟器还是 FM225，
     * 由构建选项决定，这里不做任何区分。 */
    if(face_service_init(NULL) == SAFE_OK) {
        face_service_subscribe(on_face_event, NULL);
        face_service_start();
    }
}

/* 识别帧分流：核心原则是「绝不重放旧识别」
 *  - IDLE / DETECTING：正常受理；
 *  - WAIT_OTP：只接受升级，即同一待确认用户的高置信度帧可直接放行，
 *    其余帧丢弃。真机连续出帧，若一律丢弃，用户刷脸成功也要干等超时；
 *  - UNLOCKED / DENY / LOCKOUT 等暂态：直接丢弃，否则开锁动画期间
 *    检测到的人脸会在回到 IDLE 后被重放误触发。 */
static void handle_detect_frame(int face_id, int score)
{
    if(s_fsm.state == FSM_IDLE || s_fsm.state == FSM_DETECTING) {
        auth_fsm_submit_detect(face_id, score);
    }
    else if(s_fsm.state == FSM_WAIT_OTP && s_fsm.pending_user[0]) {
        safe_user_t u;
        if(score >= user_policy()->score_high &&
           user_find_by_face(face_id, &u) == 0 &&
           strcmp(u.name, s_fsm.pending_user) == 0) {
            s_fsm.last_score = score;
            s_fsm.pending_user[0] = '\0';
            do_unlock(u.name);
        }
    }
    /* 其余状态：丢弃 */
}

static void on_face_event(face_event_t ev, const void * payload, void * user)
{
    (void)user;
    if(ev != FACE_EV_DETECT || payload == NULL) return;
    const face_result_t * fr = (const face_result_t *)payload;
    handle_detect_frame(fr->face_id, fr->score);
}

void auth_fsm_set_ui_hook(auth_ui_hook_t hook) { s_ui_hook = hook; }
void auth_fsm_set_event_cb(auth_event_cb_t cb) { s_evt_cb = cb; }

static void emit_event(const char *evt, const char *user, const char *detail, int res)
{
    if (s_evt_cb) s_evt_cb(evt, user ? user : "", detail ? detail : "", res);
}

/* 设备级失败计数 + 锁定判定；user 为失败归属（可为 NULL） */
static void note_fail(const char *user, const char *detail)
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
    } else {
        set_state(FSM_DENY, user, detail);
    }
}

static void do_unlock(const char *user)
{
    safe_user_t u;
    if (user_find_by_name(user, &u) != 0) { note_fail(user, "用户不存在"); return; }

    hal_actuator_pulse(500);   /* NFR-7：硬性 500ms 上限，截断在实现里 */

    char detail[64];
    snprintf(detail, sizeof(detail), "score=%d", s_fsm.last_score);
    log_append("UNLOCK", user, 1, detail);
    emit_event("UNLOCK", user, "face/otp ok", 1);

    /* 临时用户次数计数；用完自动失效（FR-9：保留记录，置 disabled） */
    if (strcmp(u.role, "temp") == 0 && u.use_limit > 0) {
        u.used_count++;
        if (u.used_count >= u.use_limit) {
            u.enabled = false;
            log_append("ALARM", user, 1, "临时授权次数已用尽，已自动停用");
            emit_event("ALARM", user, "temp used up", 1);
        }
    }

    u.failed_attempts = 0;
    u.lock_until = 0;
    user_update(&u);

    s_fsm.fail_streak = 0;
    set_state(FSM_UNLOCKED, user, detail);
}

/* 解析一次识别结果：user 为 NULL 表示陌生人 */
static void resolve(int face_id, int score, safe_user_t *user)
{
    const safe_policy_t *p = user_policy();
    (void)face_id;   /* 命中用户已由 user_find_by_face 解析，此处仅保留接口以备审计 */

    if (!user) {
        note_fail(NULL, "未注册人脸");
        return;
    }
    if (!user->enabled) {
        note_fail(user->name, "用户已停用");
        return;
    }
    if (!user->face_enable) {
        note_fail(user->name, "人脸通道未启用");
        return;
    }
    /* 临时用户有效性（时间/次数） */
    if (strcmp(user->role, "temp") == 0) {
        const uint32_t now = hal_time();
        if (user->valid_until > 0 && (int64_t)now >= user->valid_until) {
            note_fail(user->name, "临时授权已过期");
            return;
        }
        if (user->use_limit > 0 && user->used_count >= user->use_limit) {
            note_fail(user->name, "临时授权次数已用尽");
            return;
        }
    }

    if (score >= p->score_high) {
        do_unlock(user->name);
    } else if (score >= p->score_mid) {
        strncpy(s_fsm.pending_user, user->name, sizeof(s_fsm.pending_user) - 1);
        s_fsm.pending_user[sizeof(s_fsm.pending_user) - 1] = '\0';
        set_state(FSM_WAIT_OTP, user->name, "请使用动态密码二次确认");
    } else {
        note_fail(user->name, "置信度不足");
    }
}

void auth_fsm_submit_detect(int face_id, int score)
{
    if (score < 0 || score > 100) return;   /* 非法值忽略（不触发迁移） */
    if (s_fsm.state == FSM_LOCKOUT) return;

    s_fsm.last_score = score;
    safe_user_t u;
    safe_user_t *pu = (user_find_by_face(face_id, &u) == 0) ? &u : NULL;
    resolve(face_id, score, pu);
}

void auth_fsm_submit_pin(const char *pin)
{
    if (s_fsm.state == FSM_LOCKOUT) return;
    char user[32] = {0};
    int r = backend_verify_pin(pin, user, sizeof(user));
    if (r == UNLOCK_OK) {
        s_fsm.last_score = -1;
        do_unlock(user);
    } else if (r == UNLOCK_LOCKED) {
        note_fail(NULL, "设备已锁定");
    } else {
        note_fail(user[0] ? user : NULL, "PIN 错误");
    }
}

void auth_fsm_submit_otp(const char *code)
{
    if (s_fsm.state != FSM_WAIT_OTP) return;
    if (!s_fsm.pending_user[0]) { set_state(FSM_DENY, NULL, "非法状态"); return; }

    auth_result_t r = backend_verify_totp(s_fsm.pending_user, code);
    char name[32];
    strncpy(name, s_fsm.pending_user, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    s_fsm.pending_user[0] = '\0';

    if (r == AUTH_OK) {
        s_fsm.last_score = -1;
        do_unlock(name);
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
    s_fsm.last_score = -1;
    do_unlock(user);
}

void auth_fsm_note_failure(const char *detail)
{
    note_fail(NULL, detail ? detail : "验证失败");
}

/* ---------------- 主线程 tick ---------------- */
void auth_fsm_tick(void)
{
    const uint32_t now = hal_time();

    /* 识别结果不再在这里轮询，改由 face_service 的事件回调驱动（见 handle_detect_frame）。
     * tick 只负责状态超时推进。 */
    uint32_t elapsed = now - s_fsm.ts_enter;
    switch (s_fsm.state) {
        case FSM_DETECTING:
            if (elapsed >= TO_DETECTING) set_state(FSM_IDLE, NULL, "识别超时");
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
int auth_fsm_last_score(void) { return s_fsm.last_score; }
const char * auth_fsm_pending_user(void) { return s_fsm.pending_user; }
int auth_fsm_lock_remaining(void)
{
    if (s_fsm.state != FSM_LOCKOUT) return 0;
    int left = (int)(user_policy()->lock_seconds - (hal_time() - s_fsm.ts_enter));
    return left < 0 ? 0 : left;
}
