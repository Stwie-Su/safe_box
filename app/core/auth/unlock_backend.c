/**
 * @file unlock_backend.c
 * 解锁后端实现：多用户三通道校验的编排中枢（DESIGN.md §2 / 需求 FR-1）。
 *
 *  - PIN：匹配任一启用且未锁定的用户（PBKDF2-SHA256 哈希比对）；
 *  - 动态码：RFC 6238 + ±1 窗口容忍 + 一码一用防重放（NFR-3）；
 *  - 人脸：由 auth_fsm 调用本模块的状态机完成（见 core/auth_fsm.c），此处提供 TOTP 校验原语。
 *
 * 防暴力：失败按用户独立计数（ADR-3），达阈值锁定 lock_seconds 秒；成功清零。
 */
#include "core/auth/unlock_backend.h"
#include "core/store/store.h"
#include "core/auth/totp.h"
#include "hal/hal_time.h"
#include "hal/hal_face.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---------------- FR-9 临时授权 ---------------- */

/* 临时用户有效性（时间 + 次数），过期/超限返回 false */
static bool temp_valid(const safe_user_t *u, uint32_t now)
{
    if (strcmp(u->role, "temp") != 0) return true;
    if (u->valid_until > 0 && (int64_t)now >= u->valid_until) return false;
    if (u->use_limit  > 0 && u->used_count >= u->use_limit) return false;
    return true;
}

/* 临时授权到期 / 次数用尽 → 按需求 §5.6 直接删除该用户（含其全部凭据），动作记审计。
 * 与 core/auth/auth_fsm.c resolve() 的到期 / 用尽分支同一语义，只是触发通道不同
 * （FSM 走人脸，此处走 PIN）。授权失效不是暴力尝试，不计失败数。 */
static void temp_revoke(const safe_user_t *u, uint32_t now)
{
    const bool expired = (u->valid_until > 0 && (int64_t)now >= u->valid_until);
    char why[96];
    snprintf(why, sizeof(why), "临时授权%s，用户已删除", expired ? "已过期" : "次数已用尽");
    {   /* 级联删除模组模板（含 uid 归属核验，见 user_del_cascade） */
        int32_t fid_rm = -1;
        user_del_cascade(u->id, &fid_rm);
        if (fid_rm >= 0) face_service_delete_async(fid_rm);
    }
    log_append("ALARM", u->name, 1, why);
}

/* ---------------- FR-18 虚位密码 ---------------- */

/* 输入串中「连续包含」正确 PIN 即通过（规约 §5.2）。
 *
 * 语义（v1.8 把原文的两条隐含写成明文，未新增规则）：
 *  - 干扰位加在正确 PIN 的前后不影响连续性 → 通过；
 *  - 干扰位插进 PIN 中间会让 PIN 不再连续出现 → 不通过；
 *  - 输入超过 SAFE_VIRTUAL_PIN_MAX_INPUT（v1.9 起 12）位不受理（**后端自夹**，v1.9 补 H1 越界修复）：
 *    pin_match_virtual 把滑窗两端夹到 [1,12]，调用方无需再拦；page_keypad 的输入夹取只是第一道防线。
 *
 * 实现：先试整串（用户没加干扰位时的常见情形，命中即返回，不必跑滑窗），
 *       再按候选长度 L ∈ [pin_min_len, pin_max_len] 逐起点滑窗，命中即返回。
 * 代价（单次 PBKDF2：-O2 约 13.4ms、Debug 约 42ms；板子单核 A7 按 3~5× 估）：
 *       v1.9 单用户最坏候选数（12 位，pin_min=4/pin_max=8）= Σ_{L=4..8}(12-L+1)
 *       = 9+8+7+6+5 = 35（原 20 位为 75，约减半）。
 *       ——12 位不命中、3 用户在 PC Debug 下约 35×3×42ms ≈ 4.4s（原 20 位约 9.5s）。
 *       规约 §5.5 已把「一次长输入相当于同时试多个候选」记为已知代价，靠失败锁定对冲；
 *       v1.9 另做两点治理：①长度上限 20→12（压缩放大倍数）；②开锁 PIN 判定下沉到
 *       worker 线程——auth_fsm_submit_pin 已改为异步，界面在校验期间显示「校验中」，
 *       不再冻结主线程。注意：**不要**用「降低 PBKDF2 迭代次数」换速度（会削弱 NFR-2）。
 */
static bool pin_match_virtual(const char * input, const safe_user_t * u,
                              int min_len, int max_len)
{
    const size_t len = strlen(input);
    if (min_len < 1) min_len = 1;
    /* H1（QA ASan 实锤）：cand[] 只有 SAFE_VIRTUAL_PIN_MAX_INPUT+1 字节，而滑窗长度
     * 上界原样取策略 pin_max_len——它可由 users.json 改成任意 >12 的值，且上游闸门
     * 用的是 max(pin_max_len,12)，于是 >12 的输入被放行 → cand[L] 越界写。
     * 这里把窗口两端一律夹到 [1, SAFE_VIRTUAL_PIN_MAX_INPUT]，保证不变式成立、缓冲绝不越界
     * （store 读盘侧另有一道钳制，见 store.c load_users）。 */
    if (min_len > SAFE_VIRTUAL_PIN_MAX_INPUT) min_len = SAFE_VIRTUAL_PIN_MAX_INPUT;
    if (max_len > SAFE_VIRTUAL_PIN_MAX_INPUT) max_len = SAFE_VIRTUAL_PIN_MAX_INPUT;
    if (max_len < min_len) max_len = min_len;

    /* 整串本身就在 PIN 合法长度区间内：先做一次精确比对 */
    if (len >= (size_t)min_len && len <= (size_t)max_len &&
        pin_check(input, u->pin_salt, u->pin_hash) == 0) {
        return true;
    }

    char cand[SAFE_VIRTUAL_PIN_MAX_INPUT + 1];
    for (size_t L = (size_t)min_len; L <= (size_t)max_len && L < len; L++) {
        for (size_t off = 0; off + L <= len; off++) {
            memcpy(cand, input + off, L);
            cand[L] = '\0';
            if (pin_check(cand, u->pin_salt, u->pin_hash) == 0) return true;
        }
    }
    return false;
}

/* 单用户命中判定：allow_virtual 为真走「连续子串」，否则走 PBKDF2 精确比对。 */
static bool pin_hit(const char * pin, const safe_user_t * u, bool allow_virtual,
                    const safe_policy_t * pol)
{
    if (!allow_virtual) return pin_check(pin, u->pin_salt, u->pin_hash) == 0;
    return pin_match_virtual(pin, u, pol->pin_min_len, pol->pin_max_len);
}

unlock_result_t backend_verify_pin_ex(const char *pin, char *out_user, size_t user_cap,
                                     bool allow_virtual)
{
    if (!pin || !*pin) return UNLOCK_FAIL;

    /* 策略取快照：user_policy() 返回的是内部静态对象，取一份本地副本，
     * 避免后续 store 调用（user_update / user_del 内部可能再取策略）把它改掉。 */
    const safe_policy_t pol = *user_policy();

    const size_t len = strlen(pin);
    size_t max_input = (size_t)(pol.pin_max_len > 0 ? pol.pin_max_len : 0);
    if (allow_virtual && max_input < (size_t)SAFE_VIRTUAL_PIN_MAX_INPUT)
        max_input = (size_t)SAFE_VIRTUAL_PIN_MAX_INPUT;
    /* 长度闸门：短于 PIN 下限必不可能命中；超过上限（虚位 12 位 / 否则 pin_max_len）
     * 按规约 §5.2「超过 12 位的输入不受理」直接判失败，不进入 PBKDF2。 */
    if (len < (size_t)pol.pin_min_len || (pol.pin_max_len > 0 && len > max_input))
        return UNLOCK_FAIL;

    safe_user_t *us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0 || n <= 0) {
        user_list_free(us);
        return UNLOCK_FAIL;
    }

    const long now = (long)hal_time();
    /* 注：max_failed / lock_secs 已随「全体记失败」循环一并移除（QA-05）；
     * 设备级锁定参数由 auth_fsm 的 bump_fail_streak 从策略里自行取用。 */
    int active = 0;      /* 未锁定启用用户数 */

    /* 第一遍：尝试匹配 */
    for (int i = 0; i < n; i++) {
        if (!us[i].enabled || us[i].lock_until > now) continue;
        active++;
        if (!pin_hit(pin, &us[i], allow_virtual, &pol)) continue;

        /* FR-9 / §5.6：PIN 命中了，但该临时授权已到期或次数用尽 →
         * 删除该用户（含全部凭据）并拒绝开锁。与 auth_fsm 人脸通道的处理保持一致。 */
        if (!temp_valid(&us[i], (uint32_t)now)) {
            temp_revoke(&us[i], (uint32_t)now);
            user_list_free(us);
            return UNLOCK_FAIL;
        }

        us[i].failed_attempts = 0;
        user_update(&us[i]);
        if (out_user && user_cap) {
            strncpy(out_user, us[i].name, user_cap - 1);
            out_user[user_cap - 1] = '\0';
        }
        user_list_free(us);
        return UNLOCK_OK;
    }

    /* 无任何可尝试用户：整体锁定 */
    if (active == 0) {
        user_list_free(us);
        return UNLOCK_LOCKED;
    }

    /* 全部失败：**不**给任何用户记失败（QA-05）。
     *
     * 原实现给所有「启用且未锁定」的用户各记一次 failed_attempts —— 结果是
     * 5 次错误 PIN 就能把**全员**锁 30 秒：一个匿名输入即可拒绝服务。
     *
     * 为什么不该在这里记：PIN 是**全局输入**，一次错误尝试**无法归属到任何
     * 特定用户**（同一串错码对全体用户都只是「不匹配」）。给无法归属的对象
     * 记账本身就是设计错误，而且它与 auth_fsm 的计数重复 —— PIN 失败路径
     * 已经会调 note_fail()（见 auth_fsm.c 的 pin_done）。
     *
     * 防暴力由谁负责：auth_fsm 的「设备级连续失败计数」
     * （bump_fail_streak / s_fsm.fail_streak）—— 它不依赖归属，正是为这类
     * 场景设计的（规约 §5.2 双轨之一：设备级防陌生人轮流试，用户级锁特定账号）。
     * 用户级计数保留给**能确定归属**的通道：人脸（face_id → user 唯一），
     * 以及 bump_fail_streak() 中带 user 参数的路径。
     *
     * 注意：PIN 命中成功时的计数清零（上文分支）保持不变。 */
    user_list_free(us);
    return UNLOCK_FAIL;
}

unlock_result_t backend_verify_pin(const char *pin, char *out_user, size_t user_cap)
{
    /* 开锁通道：虚位是否生效由管理员开关决定（规约 §5.2「该功能可由管理员开关控制」）。 */
    return backend_verify_pin_ex(pin, out_user, user_cap, user_policy()->virtual_pin_enable);
}

unlock_result_t backend_verify_admin_pin(const char *pin, char *out_user, size_t user_cap)
{
    /* 敏感操作二次确认：恒为精确匹配，不受虚位开关影响（规约 §5.5）。 */
    return backend_verify_pin_ex(pin, out_user, user_cap, false);
}

auth_result_t backend_verify_totp(const char *user, const char *code)
{
    if (!user || !code) return AUTH_FAIL;
    safe_user_t u;
    if (user_find_by_name(user, &u) != 0) return AUTH_NOUSER;
    if (!u.enabled)                 return AUTH_DISABLED;
    if (!u.totp_enable)             return AUTH_CHANNEL_OFF;
    const uint32_t now = hal_time();
    if (u.lock_until > (int64_t)now) return AUTH_LOCKED;
    if (!temp_valid(&u, now))        return AUTH_EXPIRED;
    if (u.totp_secret[0] == '\0')    return AUTH_CHANNEL_OFF;

    int64_t used = 0;
    int r = totp_verify(u.totp_secret, code, now, u.last_otp_counter, &used);
    if (r == 0) {
        u.last_otp_counter = used;
        u.failed_attempts = 0;
        user_update(&u);
        return AUTH_OK;
    }
    if (r == -2) return AUTH_REPLAY;

    /* 失败：累加失败计数（防暴力），达阈值锁定 */
    u.failed_attempts++;
    const int max_failed = user_policy()->max_failed;
    const int lock_secs  = user_policy()->lock_seconds;
    if (u.failed_attempts >= max_failed) {
        u.lock_until = (int64_t)now + lock_secs;
        u.failed_attempts = 0;
    }
    user_update(&u);
    return AUTH_FAIL;
}

auth_result_t backend_verify_totp_any(const char *code, char *out_user, size_t user_cap)
{
    if (!code || !*code) return AUTH_FAIL;
    safe_user_t *us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0 || n <= 0) { user_list_free(us); return AUTH_NOUSER; }

    const uint32_t now = hal_time();
    auth_result_t res = AUTH_FAIL;

    for (int i = 0; i < n; i++) {
        if (!us[i].enabled || !us[i].totp_enable) continue;
        if (us[i].totp_secret[0] == '\0')         continue;
        if (us[i].lock_until > (int64_t)now)      continue;
        if (!temp_valid(&us[i], now))             continue;

        int64_t used = 0;
        int r = totp_verify(us[i].totp_secret, code, now, us[i].last_otp_counter, &used);
        if (r == 0) {
            us[i].last_otp_counter = used;
            us[i].failed_attempts = 0;
            user_update(&us[i]);
            if (out_user && user_cap) {
                strncpy(out_user, us[i].name, user_cap - 1);
                out_user[user_cap - 1] = '\0';
            }
            res = AUTH_OK;
            break;
        }
        if (r == -2) { res = AUTH_REPLAY; break; }
        /* 失败不在此处累加用户级计数：身份未定，避免一次错码误伤多个用户；
         * 设备级防暴力由 auth_fsm 的 note_fail 兜底。 */
    }
    user_list_free(us);
    return res;
}

auth_result_t backend_admin_verify_totp(const char *code)
{
    if (!code) return AUTH_FAIL;
    safe_user_t *us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0 || n <= 0) { user_list_free(us); return AUTH_NOUSER; }

    const uint32_t now = hal_time();
    auth_result_t res = AUTH_FAIL;

    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].role, "admin") != 0) continue;
        if (!us[i].enabled || !us[i].totp_enable) continue;
        if (us[i].totp_secret[0] == '\0') continue;
        if (us[i].lock_until > (int64_t)now) continue;

        int64_t used = 0;
        int r = totp_verify(us[i].totp_secret, code, now, us[i].last_otp_counter, &used);
        if (r == 0) {
            us[i].last_otp_counter = used;
            us[i].failed_attempts = 0;
            user_update(&us[i]);
            res = AUTH_OK;
            break;
        }
        /* 不在此处累加失败计数：管理员 TOTP 失败由调用方按全局失败处理 */
    }
    user_list_free(us);
    return res;
}

bool backend_is_locked(void)
{
    const long now = (long)hal_time();
    safe_user_t *us = NULL;
    int n = 0;
    bool locked = false;
    if (user_load_all(&us, &n) == 0) {
        for (int i = 0; i < n; i++) {
            if (us[i].enabled && us[i].lock_until > now) { locked = true; break; }
        }
    }
    user_list_free(us);
    return locked;
}

int backend_lock_remaining(void)
{
    const long now = (long)hal_time();
    long max_left = 0;
    safe_user_t *us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) == 0) {
        for (int i = 0; i < n; i++) {
            if (us[i].enabled && us[i].lock_until > now) {
                long left = us[i].lock_until - now;
                if (left > max_left) max_left = left;
            }
        }
    }
    user_list_free(us);
    return (int)max_left;
}

