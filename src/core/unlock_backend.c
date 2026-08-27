/**
 * @file unlock_backend.c
 * 解锁后端实现：多用户 PIN 校验（DESIGN.md §2）。
 *
 * 语义：
 *  - 输入 PIN 与所有【启用且未锁定】用户逐一比对（PBKDF2-SHA256 哈希比对）；
 *  - 任一匹配成功：该用户 failed_attempts 清零并落盘；
 *  - 全部不匹配：未锁定用户各自 failed_attempts+1，达阈值即锁定 lock_seconds 秒；
 *  - 所有启用用户都在锁定：返回 UNLOCK_LOCKED。
 */
#include "unlock_backend.h"
#include "store.h"
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

    const long now = (long)time(NULL);
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

bool backend_is_locked(void)
{
    const long now = (long)time(NULL);
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
    const long now = (long)time(NULL);
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
