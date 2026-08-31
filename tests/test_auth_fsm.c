/**
 * @file test_auth_fsm.c
 * 置信度分级与失败锁定用例（测试计划 1.1 / 1.3）。
 *
 * 不依赖 LVGL；执行器走模拟后端（只打印），识别结果直接注入状态机。
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "core/auth/auth_fsm.h"
#include "core/auth/totp.h"
#include "core/store/store.h"
#include "hal/hal_time.h"

#define TEST_DATA_DIR "/tmp/safe_test_auth"

static void setup_store(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DATA_DIR, TEST_DATA_DIR);
    if(system(cmd) != 0) {}
    store_set_dir(TEST_DATA_DIR);
    store_init();

    /* 两名用户：alice（user，face_id=7，绑定 TOTP）、bob（temp，face_id=8，限次 2） */
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strcpy(u.name, "alice");
    strcpy(u.role, "user");
    uint8_t salt[16];
    pin_hash("1234", salt, u.pin_hash);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled      = true;
    u.face_id      = 7;
    u.face_enable  = true;
    u.totp_enable  = true;
    CHECK(totp_secret_generate(u.totp_secret) == 0);
    u.last_otp_counter = 0;
    CHECK(user_add(&u) == 0);

    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strcpy(u.name, "bob");
    strcpy(u.role, "temp");
    pin_hash("5678", salt, u.pin_hash);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled      = true;
    u.face_id      = 8;
    u.face_enable  = true;
    u.use_limit    = 2;
    CHECK(user_add(&u) == 0);
}

static void fsm_reset(void)
{
    auth_fsm_init();
}

int main(void)
{
    setup_store();
    fsm_reset();

    /* ---- 1.1 置信度边界 ---- */
    auth_fsm_submit_detect(7, 59);
    CHECK(auth_fsm_state() == FSM_DENY);

    fsm_reset();
    auth_fsm_submit_detect(7, 60);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);

    fsm_reset();
    auth_fsm_submit_detect(7, 75);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);

    fsm_reset();
    auth_fsm_submit_detect(7, 84);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);

    fsm_reset();
    auth_fsm_submit_detect(7, 85);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    fsm_reset();
    auth_fsm_submit_detect(7, 100);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    /* 非法分数不触发迁移 */
    fsm_reset();
    auth_fsm_submit_detect(7, -1);
    CHECK(auth_fsm_state() == FSM_IDLE);
    auth_fsm_submit_detect(7, 101);
    CHECK(auth_fsm_state() == FSM_IDLE);

    /* 陌生人（无 face_id 匹配）→ 拒绝 */
    fsm_reset();
    auth_fsm_submit_detect(-1, 95);
    CHECK(auth_fsm_state() == FSM_DENY);

    /* ---- 中分区走 TOTP 后开锁 ---- */
    fsm_reset();
    auth_fsm_submit_detect(7, 75);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);
    {
        safe_user_t alice;
        CHECK(user_find_by_name("alice", &alice) == 0);
        char code[8] = {0};
        /* 状态机内部用 hal_time() 校验，这里必须用同一时间源生成当前窗口的码 */
        uint64_t now = hal_time();
        CHECK(totp_at_time(alice.totp_secret, now, 6, code, sizeof(code)) == 0);
        /* alice 的 last_otp_counter=0，当前时间片必然大于 0，可通过 */
        auth_fsm_submit_otp(code);
        CHECK(auth_fsm_state() == FSM_UNLOCKED);
    }

    /* ---- 1.3 失败锁定（设备级） ---- */
    fsm_reset();
    auth_fsm_note_failure("t1");
    auth_fsm_note_failure("t1");
    auth_fsm_note_failure("t1");
    auth_fsm_note_failure("t1");
    CHECK(auth_fsm_state() != FSM_LOCKOUT);          /* 4 次不锁 */
    auth_fsm_note_failure("t1");
    CHECK(auth_fsm_state() == FSM_LOCKOUT);          /* 第 5 次锁定 */
    CHECK(auth_fsm_lock_remaining() > 0);

    fsm_reset();
    CHECK(auth_fsm_state() == FSM_IDLE);             /* 重置后回到待机 */

    /* ---- 临时用户限次 ---- */
    fsm_reset();
    auth_fsm_submit_detect(8, 90);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);
    safe_user_t bob;
    CHECK(user_find_by_name("bob", &bob) == 0);
    CHECK(bob.used_count == 1);

    auth_fsm_submit_detect(8, 90);
    CHECK(user_find_by_name("bob", &bob) == 0);
    CHECK(bob.used_count == 2);
    CHECK(bob.enabled == false);                     /* 次数用尽自动停用 */
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    /* 停用后再刷脸 → 拒绝 */
    auth_fsm_submit_detect(8, 90);
    CHECK(auth_fsm_state() == FSM_DENY);

    /* ---- PIN 通道 ---- */
    fsm_reset();
    auth_fsm_submit_pin("1234");
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    fsm_reset();
    auth_fsm_submit_pin("0000");
    CHECK(auth_fsm_state() == FSM_DENY);

    TEST_RESULT();
}
