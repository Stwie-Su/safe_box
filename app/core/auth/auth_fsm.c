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
 * ★ 5) 认证路径零同步落盘（技术路线规约 §3.1，2026-10-07 落地）：
 *     - **读**：用户表一律走 store_cache 只读快照，不调 user_find_by_*（那会读盘）；
 *     - **写**：users.json 的改动一律经 worker（astore_auth_side），safe.log 经
 *       astore_append_log。本文件不再出现任何同步文件 IO。
 *     唯一的例外是 hal_actuator_pulse()——物理开锁必须当拍执行，不能等落盘。
 *
 * 状态超时：
 *   DETECTING face_verify_timeout_s（策略，默认 10s）/ WAIT_OTP 60s /
 *   UNLOCKED 30s / DENY 2s / LOCKOUT lock_seconds。
 */
#include "core/auth/auth_fsm.h"
#include "core/event_bus.h"
#include "core/store/store.h"                 /* 仅 user_policy()（策略已加锁快照，不读盘） */
#include "core/store/store_cache.h"           /* 只读用户快照：主线程查用户的唯一合法通道 */
#include "core/auth/totp.h"
#include "core/auth/auth_verify.h"
#include "core/support/worker.h"             /* worker_post：PIN / OTP 后台校验 */
#include "core/support/async_store.h"         /* astore_auth_side / astore_append_log */
#include "hal/hal_time.h"
#include "hal/hal_face.h"
#include "hal/hal_actuator.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* calloc/free：PIN / OTP 作业对象 */
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

/* 动态码后台作业的世代号发生器与「在途」标志。
 * 定义在这里（而非 submit_otp 附近）是因为 set_state() 也要访问它——见那里的注释。 */
static uint32_t s_otp_gen    = 0;   /* 提交计数（世代号发生器） */
static uint32_t s_otp_active = 0;   /* 当前在途作业的世代号；0 = 无 */

/* 人脸事件入口：hal_face_emit → event_bus(EV_FACE_EVENT) → 本回调（主线程） */
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
/* TO_VERIFY：PIN 后台校验的安全网超时（秒）。仅用于「worker 异常未回调」的兜底，
 * 正常路径不会命中（回调到达即离开 FSM_VERIFYING）。取 120s，远大于最慢 PBKDF2
 * （12 位虚位 × 全部用户在单核 A7 上的耗时）。 */
#define TO_VERIFY    120
/* LOCKOUT 用策略 lock_seconds（阶段 1 默认 30） */

static void set_state(fsm_state_t st, const char *user, const char *detail)
{
    s_fsm.state = st;
    s_fsm.ts_enter = hal_time();
    /* ★ 离开 WAIT_OTP 即作废在途的动态码作业。
     * 复位**统一放在这里**，而不是散在各条离开路径（tick 超时 / 手动取消 /
     * WAIT_OTP 期间刷脸放行 / 校验失败……）—— 漏掉任何一条都会让
     * s_otp_active 永久非 0，submit_otp 从此被永久忽略。
     * 当初不做单飞正是因为怕这个散落风险；把它收进 set_state 之后，
     * 单飞就不再有卡死隐患，可以安全启用。 */
    if (st != FSM_WAIT_OTP) s_otp_active = 0;
    if (user) { strncpy(s_fsm.last_user, user, sizeof(s_fsm.last_user) - 1); s_fsm.last_user[sizeof(s_fsm.last_user)-1] = '\0'; }
    if (s_ui_hook) s_ui_hook(st, user ? user : "", detail ? detail : "");
}

void auth_fsm_init(void)
{    memset(&s_fsm, 0, sizeof(s_fsm));
    s_fsm.state = FSM_IDLE;
    s_fsm.last_reason = FACE_RES_ERROR;
    /* 在途动态码作业一并作废：否则 init 之后残留的 s_otp_active 会让
     * submit_otp 被永久忽略（单飞判据读的就是它）。 */
    s_otp_gen    = 0;
    s_otp_active = 0;

    /* 人脸通道（步骤 3a / R6 接线）：订阅总线 EV_FACE_EVENT。
     * 后端结果由 hal_face_emit 广播（主线程 publish 同步派发），
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
            if(face_id >= 0 && store_cache_find_by_face(face_id, &u) == 0) {
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
            astore_append_log("ALARM", "-", 0, "活体检测失败（防伪）");
            emit_event("ALARM", "-", "liveness fail", 0);
            if(!bump_fail_streak(NULL, "活体检测失败"))
                set_state(FSM_DENY, NULL, "活体检测失败");
        }
        /* TIMEOUT / ERROR：不计数不迁移 */
    }
    /* 其余状态：丢弃 */
}

/* 人脸事件回调（总线签名）：payload 为 ev_face_event_t（POD，步骤 3a），
 * 由 hal_face_emit → event_bus_publish 在主线程同步派发。 */
static void on_face_event(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if(topic != EV_FACE_EVENT || payload == NULL) return;
    const ev_face_event_t * e = (const ev_face_event_t *)payload;

    /* 人脸录入成功 → 清零**设备级**失败计数（用户要求 2026-09-20）。
     *
     * 为什么要清：录入需要管理员 PIN 鉴权 + 当面采集，用户此刻已充分证明身份；
     * 而 fail_streak 是设备级计数（跨用户累积、不区分是谁失败的）。不清零的话，
     * 录入/测试过程里累积的几次未匹配会立刻把用户推进 WAIT_OTP
     * （face_otp_after 默认只有 3）——「录完想马上试一下」必然撞墙。
     * 这是纯体验修复：**不降低防暴力强度**（真正的暴力尝试不会先通过管理员鉴权
     * 再录一张脸；而正常开锁成功本来就会在本文件 do_unlock 里清零）。 */
    if (e->ev == FACE_EV_ENROLL_DONE && e->enroll.err == SAFE_OK) {
        s_fsm.fail_streak = 0;
        return;   /* ENROLL 与认证状态机其余逻辑无关，处理完即返回 */
    }

    if(e->ev != FACE_EV_DETECT) return;   /* DELETE/ERROR 与认证状态机无关 */
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
 * 返回 true 表示已达 max_failed 并进入 LOCKOUT（调用方不应再改状态）。
 *
 * ★ 为什么「设备级计数」留在内存、只有「每用户计数」下沉 worker：
 *   设备级 fail_streak 是**纯内存状态**，不落盘——它必须当拍生效，否则
 *   LOCKOUT 的 UI 迁移就会晚一帧，而这正是防暴力的即时性所在。
 *   每用户 failed_attempts / lock_until 要落盘，且是「读→改→写」三步，
 *   必须整段交给 worker（主线程基于快照做加减会丢失更新，见 async_store.h）。
 *   worker 单线程 FIFO 保证：本次落盘完成后，后续 PIN 校验作业才会读到新值。 */
static bool bump_fail_streak(const char *user, const char *detail)
{
    const safe_policy_t *p = user_policy();
    s_fsm.fail_streak++;

    /* 每用户计数（用于 PIN 通道单独锁定 / 审计）→ 读改写下沉 worker */
    if (user && *user) astore_auth_side(AUTH_SIDE_FAIL_BUMP, user, NULL, NULL);

    astore_append_log("DENY", user ? user : "-", 0, detail ? detail : "");
    emit_event("DENY", user, detail, 0);

    if (s_fsm.fail_streak >= p->max_failed) {
        s_fsm.fail_streak = 0;
        char buf[48];
        snprintf(buf, sizeof(buf), "连续 %d 次失败，锁定 %ds", p->max_failed, p->lock_seconds);
        astore_append_log("LOCKOUT", "-", 0, buf);
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

/* 级联删除类作业的完成回调（主线程）：唯一要补做的是「通知 face 线程删模组模板」。
 * 那是异步投递（不落盘、不阻塞），且必须在主线程——hal_face_delete_async 会走
 * 模组命令队列。result / removed 只影响审计文案，而审计已由 worker 侧写好了。 */
static void revoke_done(int result, int32_t face_id_rm, bool removed)
{
    (void)result; (void)removed;
    if (face_id_rm >= 0) hal_face_delete_async(face_id_rm);
}

static void do_unlock(const char *user, const char *via)
{
    safe_user_t u;
    if (store_cache_find_by_name(user, &u) != 0) { note_fail(user, "用户不存在"); return; }

    hal_actuator_pulse(500);   /* NFR-7：硬性 500ms 上限，截断在实现里 */

    char detail[64];
    snprintf(detail, sizeof(detail), "via=%s", via ? via : "?");
    astore_append_log("UNLOCK", user, 1, detail);
    emit_event("UNLOCK", user, detail, 1);

    /* 用户表副作用下沉 worker（FR-9 临时用户计数 / 用尽即删 + 成功清零计数）。
     * 「次数用尽已删除」的审计日志由 worker 在真正删除后补记 —— 主线程此刻
     * 只知道快照里的 used_count，不能替 worker 下结论。 */
    astore_auth_side(AUTH_SIDE_UNLOCK_TOUCH, user,
                     "临时授权次数已用尽，用户已删除", revoke_done);

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
    astore_append_log("ALARM", user ? user->name : "-", 0, "活体检测失败（防伪）");
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
    astore_append_log("ALARM", u->name, 0, detail);
    emit_event("ALARM", u->name, "credential mismatch", 0);

    /* 本地人脸凭据标记失效（FR-21）。下沉 worker：它在 worker 内判断
     * face_enable 是否仍为真（幂等，已失效就不写盘），并在真正改写后
     * 补记「已标记失效」审计 —— 主线程不该替它下这个结论。 */
    astore_auth_side(AUTH_SIDE_FACE_INVALIDATE, u->name, NULL, NULL);
    return false;
}

/* 解析一次人脸结果：user 为 NULL 表示模组未给出匹配身份 */
static void resolve(int32_t face_id, face_reason_t reason, safe_user_t *user,
                    const char * mod_name)
{
    (void)face_id;   /* 命中用户已由调用方按模板号在只读快照里解析，此处仅保留接口以备审计 */

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
            /* 级联删除下沉 worker（含 uid 归属核验，见 user_del_cascade）；
             * 审计文案 why 随作业带过去，由 worker 在真正删除后落盘，
             * 主线程 done 回调只补「通知 face 线程删模组模板」。 */
            astore_auth_side(AUTH_SIDE_TEMP_REVOKE, user->name, why, revoke_done);
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
    /* 查用户走只读快照：主线程不读盘。快照是「上一次落盘后的状态」，
     * 用于判「这张脸是谁 / 该用户是否启用 / 通道是否开」——这些不因一帧滞后
     * 而改变安全结论（真正决定锁定的计数读写已整体下沉 worker）。 */
    safe_user_t *pu = (face_id >= 0 && store_cache_find_by_face(face_id, &u) == 0) ? &u : NULL;
    /* mod_name 直接穿透给 resolve（本函数在主线程被 event_bus_pump 调用，无并发问题） */
    resolve(face_id, reason, pu, mod_name);
}

/* ---------------- 开锁 PIN 异步校验（v1.9） ----------------
 * 判定下沉 worker 线程；作业对象自带「世代号」，回调据此丢弃陈旧结果。
 * L2（QA）：仅凭 state==FSM_VERIFYING 无法区分「超时作废的旧作业」与「新会话的作业」——
 * 若旧作业被前序任务堵塞超过安全网超时、期间又发生新提交，旧回调到达时状态恰为 VERIFYING，
 * 会把旧结果当成新会话的。世代号消除这一缺口。
 * 注：不依赖 async_store 的 PIN 包装（其回调签名无上下文槽、无法携带世代号），
 * 直接以 worker_post 自建带 gen 的作业（同时去掉 core/auth→core/support 的依赖）。 */
typedef struct {
    uint32_t gen;                             /* 世代号：每次提交自增 */
    char     pin[SAFE_VIRTUAL_PIN_MAX_INPUT + 1];
    int      result;                          /* unlock_result_t */
    char     user[32];
} pin_job_t;

static uint32_t s_pin_gen    = 0;             /* 提交计数（世代号发生器） */
static uint32_t s_pin_active = 0;             /* 当前在途作业的世代号；0 = 无 */
static uint32_t s_verify_timeout = TO_VERIFY; /* 安全网超时（秒），可调，默认 120 */

/* 后台线程：跑 PBKDF2（含虚位滑窗）。绝不触碰 UI / LVGL。 */
static void pin_worker(void * p)
{
    pin_job_t * a = (pin_job_t *)p;
    a->user[0] = '\0';
    a->result = (int)auth_verify_pin(a->pin, a->user, sizeof(a->user));
}

/* 主线程（worker_poll 派发）：仅当「世代号 == 当前在途作业」且状态仍为 VERIFYING 时才受理。
 * 世代号挡住「超时作废后迟到的旧作业」，状态挡住在其它通道已推进后到达的旧结果；
 * 任一不符即丢弃，绝不误开锁 / 误计失败。 */
static void pin_done(void * p)
{
    pin_job_t * a = (pin_job_t *)p;
    if (a->gen == s_pin_active && s_fsm.state == FSM_VERIFYING) {
        if (a->result == UNLOCK_OK) {
            s_fsm.last_reason = FACE_RES_ERROR;   /* 本次开锁与人脸无关 */
            do_unlock(a->user, "pin");
        } else if (a->result == UNLOCK_LOCKED) {
            note_fail(NULL, "设备已锁定");
        } else {
            note_fail(a->user[0] ? a->user : NULL, "PIN 错误");
        }
    }
    free(a);   /* 作业对象由本回调释放（worker_poll 只释放节点本身） */
}

/* 调安全网超时（秒）。0 = 下一次 tick 即超时；供调优 / 测试。 */
void auth_fsm_set_verify_timeout(uint32_t seconds) { s_verify_timeout = seconds; }

/* PIN 通道入口（v1.9 异步化）：把「多用户 × 候选子串」的 PBKDF2 判定下沉到 worker 线程，
 * 主线程只做「提交 → 进入校验中 → 收结果」，消除长虚位输入导致的界面冻结。
 * 判定口径不变：走 auth_verify_pin（连续子串匹配，受 virtual_pin_enable 控制）。
 * 校验期间拒绝新的 PIN 提交（单飞），避免并发覆盖状态。 */
void auth_fsm_submit_pin(const char *pin)
{
    if (s_fsm.state == FSM_LOCKOUT) return;
    if (s_fsm.state == FSM_VERIFYING) return;   /* 上一次仍未返回：忽略重复提交 */
    if (!pin || !*pin) return;

    pin_job_t * a = (pin_job_t *)calloc(1, sizeof(*a));
    if (!a) { note_fail(NULL, "校验任务分配失败"); return; }
    a->gen = ++s_pin_gen;
    strncpy(a->pin, pin, sizeof(a->pin) - 1);
    a->pin[sizeof(a->pin) - 1] = '\0';
    s_pin_active = a->gen;

    set_state(FSM_VERIFYING, NULL, "正在校验 PIN");
    worker_post(pin_worker, a, pin_done);
}

/* ---------------- 动态码（OTP）后台校验（2026-10-07） ----------------
 * 与 PIN 通道同构：判定下沉 worker，回调带世代号。
 *
 * 为什么 OTP 也要下沉：auth_verify_totp 内部会 user_find_by_name + user_update
 * （回写 last_otp_counter 防重放计数器、累加失败计数）——那是同步读盘 + 整表写盘，
 * 直接违反规约 §3.1。更要紧的是「读 last_otp_counter → 判重放 → 写回」本身就是
 * 读改写：拆到两个线程做，同一个码就有机会被用两次。
 *
 * ★ 与 PIN 通道刻意不同的一点：**不新增 FSM 状态**——校验期间仍停在 WAIT_OTP
 *   （UI 无需改版），靠世代号区分新旧作业。
 *
 * ★ 但仍要做**单飞**（这是踩过之后改的）：世代号只能丢弃「结果」，撤销不了
 *   worker **已经执行的副作用**。若不单飞，连点两次确认会投出两个作业，
 *   第一个在 worker 里把 last_otp_counter 写回（动态码被消耗），其 done 却因
 *   世代号不匹配被丢弃 → 不开锁；第二个作业随后判 AUTH_REPLAY → 提示
 *   「动态码已使用」。净结果：**码被吃掉、门没开**。
 *   单飞的风险（在途标志卡死）由 set_state 统一复位解决，见那里。
 *   pending_user 改在回调里清空——提交后还要拿它给 UI 显示、给 note_fail 记名。 */
typedef struct {
    uint32_t gen;
    char     pending[32];   /* 空串 = 身份未定（走 auth_verify_totp_any） */
    char     code[16];
    auth_result_t result;
    char     user[32];      /* 命中的用户名（AUTH_OK 时有效） */
} otp_job_t;

/* 后台线程：TOTP 校验（HMAC-SHA1 + 防重放计数器回写 + 失败计数）。绝不触碰 UI。 */
static void otp_worker(void * p)
{
    otp_job_t * a = (otp_job_t *)p;
    a->user[0] = '\0';
    if (a->pending[0]) {
        a->result = auth_verify_totp(a->pending, a->code);
        if (a->result == AUTH_OK) {
            strncpy(a->user, a->pending, sizeof(a->user) - 1);
            a->user[sizeof(a->user) - 1] = '\0';
        }
    } else {
        /* 身份未定：在「任一启用 + TOTP 开启 + 未锁定 + 授权有效」的用户上校验 */
        a->result = auth_verify_totp_any(a->code, a->user, sizeof(a->user));
    }
}

/* 主线程（worker_poll 派发）：双判据同 pin_done —— 世代号挡住「被覆盖的旧作业」，
 * 状态挡住「其它通道已推进后到达的结果」。任一不符即丢弃，绝不误开锁 / 误计失败。 */
static void otp_done(void * p)
{
    otp_job_t * a = (otp_job_t *)p;
    const char * who = a->pending[0] ? a->pending : NULL;

    if (a->gen == s_otp_active && s_fsm.state == FSM_WAIT_OTP) {
        s_otp_active = 0;
        s_fsm.pending_user[0] = '\0';
        switch (a->result) {
        case AUTH_OK:
            s_fsm.last_reason = FACE_RES_ERROR;   /* 本次开锁与人脸无关 */
            do_unlock(a->user, "otp");
            break;
        case AUTH_REPLAY:       note_fail(who, "动态码已使用");       break;
        case AUTH_LOCKED:       note_fail(who, "用户已锁定");         break;
        case AUTH_DISABLED:     note_fail(who, "用户已停用");         break;
        case AUTH_CHANNEL_OFF:  note_fail(who, "动态码通道未启用");   break;
        case AUTH_EXPIRED:      note_fail(who, "临时授权已失效");     break;
        default:                note_fail(who, "动态码错误");         break;
        }
    }
    memset(a->code, 0, sizeof(a->code));   /* 明文动态码不残留（与 rpc.c 同纪律） */
    free(a);
}

void auth_fsm_submit_otp(const char *code)
{
    if (s_fsm.state != FSM_WAIT_OTP) return;
    if (!code || !*code) return;
    /* 单飞：上一次校验仍未返回就直接忽略，不投第二个作业。
     * 理由见本节注释——世代号挡不住「worker 已执行的副作用」。 */
    if (s_otp_active != 0) return;

    otp_job_t * a = (otp_job_t *)calloc(1, sizeof(*a));
    if (!a) { note_fail(NULL, "动态码校验任务分配失败"); return; }
    a->gen = ++s_otp_gen;
    strncpy(a->pending, s_fsm.pending_user, sizeof(a->pending) - 1);
    a->pending[sizeof(a->pending) - 1] = '\0';
    strncpy(a->code, code, sizeof(a->code) - 1);
    a->code[sizeof(a->code) - 1] = '\0';
    s_otp_active = a->gen;

    worker_post(otp_worker, a, otp_done);
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
        case FSM_VERIFYING:
            /* 安全网（异常路径，非正常流程）：正常情况下 pin_done 必定回调并离开本状态。
             * 若后台因故迟迟不返回，超时后放弃等待、回 IDLE，并作废在途世代号
             * （s_pin_active=0）——随后迟到的旧作业按世代号被丢弃，不会误开锁/误计失败。
             * 阈值（默认 120s）远大于最坏 PBKDF2 耗时，正常永不触发。 */
            if (elapsed >= s_verify_timeout) {
                s_pin_active = 0;
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
        case FSM_VERIFYING: return "VERIFYING";
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

