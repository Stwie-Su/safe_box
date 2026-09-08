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
#include <string.h>
#include <time.h>

unlock_result_t backend_verify_pin(const char *pin, char *out_user, size_t user_cap)
{
    if (!pin || !*pin) return UNLOCK_FAIL;

    safe_user_t *us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0 || n <= 0) {
        user_list_free(us);
        return UNLOCK_FAIL;
    }

    const long now = (long)hal_time();
    const int max_failed = user_policy()->max_failed;
    const int lock_secs  = user_policy()->lock_seconds;
    int active = 0;      /* 未锁定启用用户数 */

    /* 第一遍：尝试匹配 */
    for (int i = 0; i < n; i++) {
        if (!us[i].enabled || us[i].lock_until > now) continue;
        active++;
        if (pin_check(pin, us[i].pin_salt, us[i].pin_hash) == 0) {
            us[i].failed_attempts = 0;
            user_update(&us[i]);
            if (out_user && user_cap) {
                strncpy(out_user, us[i].name, user_cap - 1);
                out_user[user_cap - 1] = '\0';
            }
            user_list_free(us);
            return UNLOCK_OK;
        }
    }

    /* 无任何可尝试用户：整体锁定 */
    if (active == 0) {
        user_list_free(us);
        return UNLOCK_LOCKED;
    }

    /* 全部失败：未锁定用户各记一次失败 */
    for (int i = 0; i < n; i++) {
        if (!us[i].enabled || us[i].lock_until > now) continue;
        us[i].failed_attempts++;
        if (us[i].failed_attempts >= max_failed) {
            us[i].lock_until = now + lock_secs;
            us[i].failed_attempts = 0;
        }
        user_update(&us[i]);
    }
    user_list_free(us);
    return UNLOCK_FAIL;
}

/* 临时用户有效性（时间 + 次数），过期/超限返回 false */
static bool temp_valid(const safe_user_t *u, uint32_t now)
{
    if (strcmp(u->role, "temp") != 0) return true;
    if (u->valid_until > 0 && (int64_t)now >= u->valid_until) return false;
    if (u->use_limit  > 0 && u->used_count >= u->use_limit) return false;
    return true;
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

