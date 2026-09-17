/**
 * @file test_unlock_backend.c
 * 解锁后端用例（需求 v1.9）：
 *   - FR-18 虚位密码：前后加扰通过 / 中间插入被拒 / 超过 12 位被拒 / 开关关闭时不生效；
 *   - H1 回归：策略 pin_max_len 被手改成 >12 时，读盘钳制 + 超长输入安全拒绝（防栈越界）；
 *   - §5.5 边界：管理员二次确认恒为精确匹配，不吃虚位；
 *   - FR-9 临时授权：PIN 通道命中已过期 / 次数用尽的临时用户 → 拒绝并自动删除；
 *   - FR-3 动态码：用户 TOTP / 身份未定 TOTP / 管理员 TOTP / 一码一用防重放；
 *   - 策略开关持久化：virtual_pin_enable 改完能落盘、重启（重新读盘）后仍在。
 *
 * 不依赖 LVGL，数据落在独立临时目录。
 */
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth/unlock_backend.h"
#include "core/auth/totp.h"
#include "core/store/store.h"
#include "hal/hal_time.h"

#define TEST_DATA_DIR "/tmp/safe_test_unlock"

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

/* 建一个用户。role 必须是 admin/user/temp 之一。 */
static void add_user(const char * name, const char * role, const char * pin,
                     int64_t valid_until, int use_limit, int used_count, bool totp)
{
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strncpy(u.name, name, sizeof(u.name) - 1);
    strncpy(u.role, role, sizeof(u.role) - 1);
    strncpy(u.auth_method, "pin", sizeof(u.auth_method) - 1);
    uint8_t salt[16];
    CHECK(pin_hash(pin, salt, u.pin_hash) == 0);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled     = true;
    u.face_id     = -1;
    u.valid_until = valid_until;
    u.use_limit   = use_limit;
    u.used_count  = used_count;
    if (totp) {
        u.totp_enable = true;
        CHECK(totp_secret_generate(u.totp_secret) == 0);
    }
    CHECK(user_add(&u) == 0);
}

/* 失败计数清零：backend_verify_pin 失败会给每个未锁定用户 +1，
 * 打满 max_failed（默认 5）会进锁定，污染后续用例。用完即清。 */
static void clear_fail_counters(void)
{
    safe_user_t * us = NULL;
    int n = 0;
    if (user_load_all(&us, &n) != 0 || n <= 0) { user_list_free(us); return; }
    for (int i = 0; i < n; i++) {
        if (us[i].failed_attempts != 0 || us[i].lock_until != 0) {
            us[i].failed_attempts = 0;
            us[i].lock_until      = 0;
            user_update(&us[i]);
        }
    }
    user_list_free(us);
}

static void setup_store(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DATA_DIR, TEST_DATA_DIR);
    if (system(cmd) != 0) { /* rm/mkdir 失败会在 store_init 里暴露 */ }
    store_set_dir(TEST_DATA_DIR);
    CHECK(store_init());

    /* bootstrap 已建 admin（PIN 123456、role=admin、TOTP 已绑固定演示密钥）。
     * 下面建的用户 PIN 都不与 "123456" 构成包含关系，避免虚位滑窗误命中。 */
    add_user("alice",  "user", "2345", 0, 0, 0, false);          /* 普通用户 */
    add_user("tvalid", "temp", "3456", (int64_t)hal_time() + 3600, 0, 0, false);
    add_user("texp",   "temp", "4567", (int64_t)hal_time() - 1,    0, 0, false);
    add_user("tused",  "temp", "5678", 0, 1, 1, false);            /* 次数已用尽 */
    add_user("otpusr", "user", "6789", 0, 0, 0, true);             /* 动态码通道 */
}

/* ------------------------------------------------------------------ */

int main(void)
{
    char who[32] = {0};

    setup_store();

    /* ---------- 0. 前置：策略与角色合法性 ---------- */
    CHECK(user_role_valid("admin"));
    CHECK(user_role_valid("user"));
    CHECK(user_role_valid("temp"));
    CHECK(!user_role_valid("root"));
    CHECK(!user_role_valid(""));
    CHECK(!user_role_valid(NULL));

    const safe_policy_t * pol0 = user_policy();
    const int pin_min = pol0->pin_min_len;
    const int pin_max = pol0->pin_max_len;
    CHECK(pin_min >= 4 && pin_max >= pin_min);
    CHECK(pol0->virtual_pin_enable == true);      /* 出厂默认开启（FR-18 默认可用） */

    /* ---------- 1. PIN 基本路径 ---------- */
    who[0] = '\0';
    CHECK(backend_verify_pin("2345", who, sizeof(who)) == UNLOCK_OK);
    CHECK(strcmp(who, "alice") == 0);

    who[0] = '\0';
    CHECK(backend_verify_pin("0000", who, sizeof(who)) == UNLOCK_FAIL);
    CHECK(who[0] == '\0');
    clear_fail_counters();

    /* ---------- 2. FR-18 虚位密码：前后加扰应当通过 ---------- */
    who[0] = '\0';
    CHECK(backend_verify_pin("992345", who, sizeof(who)) == UNLOCK_OK);   /* 前缀干扰 */
    CHECK(strcmp(who, "alice") == 0);

    who[0] = '\0';
    CHECK(backend_verify_pin("234500", who, sizeof(who)) == UNLOCK_OK);   /* 后缀干扰 */
    CHECK(strcmp(who, "alice") == 0);

    who[0] = '\0';
    CHECK(backend_verify_pin("11234599", who, sizeof(who)) == UNLOCK_OK); /* 前后都有 */
    CHECK(strcmp(who, "alice") == 0);

    who[0] = '\0';
    CHECK(backend_verify_pin("234500000", who, sizeof(who)) == UNLOCK_OK);/* 9 位：超 pin_max_len */
    CHECK(strcmp(who, "alice") == 0);

    /* ---------- 3. FR-18：中间插入干扰位 → PIN 不再连续出现 → 拒绝 ---------- */
    CHECK(backend_verify_pin("23945", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();
    CHECK(backend_verify_pin("23459", NULL, 0) == UNLOCK_OK);   /* 对照：加在尾部算后缀干扰 */
    clear_fail_counters();

    /* ---------- 4. FR-18：超过 12 位不受理（v1.9：上限由 20 收紧为 12） ---------- */
    {
        char over[32];
        memset(over, '0', sizeof(over));
        /* 8 个前导 0 + "2345" = 12 位（上限内）→ 通过 */
        strcpy(over + 8, "2345");
        CHECK(strlen(over) == 12);
        CHECK(backend_verify_pin(over, NULL, 0) == UNLOCK_OK);

        /* 9 个前导 0 + "2345" = 13 位（超限）→ 拒绝 */
        memset(over, '0', sizeof(over));
        strcpy(over + 9, "2345");
        over[13] = '\0';
        CHECK(strlen(over) == 13);
        CHECK(backend_verify_pin(over, NULL, 0) == UNLOCK_FAIL);
        clear_fail_counters();
    }

    /* ---------- 5. 不含正确 PIN 的长串 → 拒绝 ---------- */
    CHECK(backend_verify_pin("1111111111", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();

    /* ---------- 6. 开关关闭 → 虚位失效，精确 PIN 仍可用 ---------- */
    user_policy_set_virtual_pin(false);
    CHECK(user_policy()->virtual_pin_enable == false);
    {
        /* 强制重新读盘（load_users 会把 policy 同步回内存）：验证开关真的落盘了。
         * 修前 store.c 只写不读，这里会退回 true —— 即管理员改的开关重启即丢。 */
        safe_user_t * us = NULL;
        int n = 0;
        CHECK(user_load_all(&us, &n) == 0);
        user_list_free(us);
        CHECK(user_policy()->virtual_pin_enable == false);
    }
    CHECK(backend_verify_pin("992345", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();
    CHECK(backend_verify_pin("234500000", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();
    CHECK(backend_verify_pin("2345", NULL, 0) == UNLOCK_OK);      /* 精确匹配不受影响 */

    /* ---------- 7. §5.5：管理员二次确认恒为精确匹配，不受虚位开关影响 ---------- */
    /* 此时开关为 false：精确通过、虚位串被拒 */
    who[0] = '\0';
    CHECK(backend_verify_admin_pin("123456", who, sizeof(who)) == UNLOCK_OK);
    CHECK(strcmp(who, "admin") == 0);
    CHECK(backend_verify_admin_pin("12345600", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();

    /* 打开虚位后再验一次：管理员路径**依然**不接受虚位 */
    user_policy_set_virtual_pin(true);
    CHECK(user_policy()->virtual_pin_enable == true);
    who[0] = '\0';
    CHECK(backend_verify_admin_pin("123456", who, sizeof(who)) == UNLOCK_OK);
    CHECK(strcmp(who, "admin") == 0);
    CHECK(backend_verify_admin_pin("12345600", NULL, 0) == UNLOCK_FAIL);
    clear_fail_counters();
    /* 同一串走开锁通道应当通过 —— 证明上面被拒确实来自「通道差异」而非实现没生效 */
    CHECK(backend_verify_pin("12345600", NULL, 0) == UNLOCK_OK);

    /* ---------- 8. FR-9：临时用户过期 → 拒绝并自动删除 ---------- */
    {
        safe_user_t u;
        CHECK(user_find_by_name("texp", &u) == 0);            /* 删除前存在 */
    }
    CHECK(backend_verify_pin("4567", NULL, 0) == UNLOCK_FAIL);
    {
        safe_user_t u;
        CHECK(user_find_by_name("texp", &u) != 0);            /* 已被自动删除 */
    }
    clear_fail_counters();

    /* ---------- 9. FR-9：临时用户次数耗尽 → 拒绝并自动删除 ---------- */
    {
        safe_user_t u;
        CHECK(user_find_by_name("tused", &u) == 0);
        CHECK(u.used_count >= u.use_limit);
    }
    CHECK(backend_verify_pin("5678", NULL, 0) == UNLOCK_FAIL);
    {
        safe_user_t u;
        CHECK(user_find_by_name("tused", &u) != 0);           /* 已被自动删除 */
    }
    clear_fail_counters();

    /* ---------- 10. FR-9：有效期内的临时用户正常开锁 ---------- */
    who[0] = '\0';
    CHECK(backend_verify_pin("3456", who, sizeof(who)) == UNLOCK_OK);
    CHECK(strcmp(who, "tvalid") == 0);

    /* ---------- 11. FR-3：用户动态码 ---------- */
    {
        safe_user_t u;
        CHECK(user_find_by_name("otpusr", &u) == 0);
        char code[8] = {0};
        uint64_t now = hal_time();
        CHECK(totp_at_time(u.totp_secret, now, 6, code, sizeof(code)) == 0);

        CHECK(backend_verify_totp("otpusr", code) == AUTH_OK);
        CHECK(backend_verify_totp("otpusr", code) == AUTH_REPLAY);   /* 一码一用 */
        CHECK(backend_verify_totp("otpusr", "000000") == AUTH_FAIL);
        clear_fail_counters();

        /* 下一个时间片的码：仍应通过（±1 窗口 + 严格大于 last_counter） */
        char code2[8] = {0};
        CHECK(totp_at_time(u.totp_secret, now + TOTP_PERIOD, 6, code2, sizeof(code2)) == 0);
        char name[32] = {0};
        CHECK(backend_verify_totp_any(code2, name, sizeof(name)) == AUTH_OK);
        CHECK(strcmp(name, "otpusr") == 0);

        /* 未启用动态码的通道：alice 没绑 TOTP */
        CHECK(backend_verify_totp("alice", code2) == AUTH_CHANNEL_OFF);
    }

    /* ---------- 12. FR-3：管理员动态码（仅认管理员） ---------- */
    {
        safe_user_t u;
        CHECK(user_find_by_name("otpusr", &u) == 0);
        char ucode[8] = {0};
        uint64_t now = hal_time();
        CHECK(totp_at_time(u.totp_secret, now + 2 * TOTP_PERIOD, 6, ucode, sizeof(ucode)) == 0);
        /* otpusr 是普通用户：它的码不能通过管理员二次确认 */
        CHECK(backend_admin_verify_totp(ucode) == AUTH_FAIL);

        safe_user_t ad;
        CHECK(user_find_by_name("admin", &ad) == 0);
        CHECK(strcmp(ad.role, "admin") == 0);
        char acode[8] = {0};
        CHECK(totp_at_time(ad.totp_secret, now, 6, acode, sizeof(acode)) == 0);
        CHECK(backend_admin_verify_totp(acode) == AUTH_OK);
        CHECK(backend_admin_verify_totp(acode) == AUTH_FAIL);        /* 重放被拒 */
        CHECK(backend_admin_verify_totp("000000") == AUTH_FAIL);
        clear_fail_counters();
    }

    /* ---------- 13. 锁定查询接口（回归） ---------- */
    CHECK(backend_is_locked() == false);
    CHECK(backend_lock_remaining() == 0);

    /* ---------- 14. H1 回归：策略 pin_max_len 越界不得导致虚位滑窗栈越界 ----------
     * QA 用 ASan 实锤：pin_match_virtual 的栈缓冲 cand[SAFE_VIRTUAL_PIN_MAX_INPUT+1] 若滑窗
     * 上界原样取 pin_max_len（users.json 可手改成 >12），超长输入会让 cand[L] 越界写。
     * 本用例把策略里的 pin_max_len 手改成 40（> 12）后重读策略，验证两道防线：
     *   (a) store 读盘钳制：policy.pin_max_len 被夹到 SAFE_VIRTUAL_PIN_MAX_INPUT；
     *   (b) 喂 40 位串不崩溃、安全拒绝（修复前会在 cand[13..40] 越界写）。
     * 放在最后，避免改动策略影响前面用例。 */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "sed -i 's/\"pin_max_len\": *[0-9]*/\"pin_max_len\": 40/' %s/users.json",
                 TEST_DATA_DIR);
        if (system(cmd) != 0) { /* 失败会在下面的断言里暴露 */ }
    }
    {
        safe_user_t * us = NULL;
        int n = 0;
        CHECK(user_load_all(&us, &n) == 0);   /* 触发 load_users：重读并钳制策略 */
        user_list_free(us);
    }
    CHECK(user_policy()->pin_max_len == SAFE_VIRTUAL_PIN_MAX_INPUT);   /* (a) 读盘钳制 */
    {
        char big[64];
        memset(big, '0', 40);
        big[40] = '\0';
        CHECK(strlen(big) == 40);
        CHECK(backend_verify_pin(big, NULL, 0) == UNLOCK_FAIL);        /* (b) 不越界、安全拒绝 */
        clear_fail_counters();
    }

    TEST_RESULT();
}
