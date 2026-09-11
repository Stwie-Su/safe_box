/**
 * @file test_auth_fsm.c
 * 按原因分流与失败锁定用例（需求 v1.6 FR-19 / 测试计划 1.1 / 1.3）。
 *
 * 不依赖 LVGL；执行器走模拟后端（只打印），识别结果直接注入状态机。
 * 注入接口为 auth_fsm_submit_face(face_id, reason)，无分数概念。
 */
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
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

    /* 两名用户：alice（user，face_id=7，绑定 TOTP）、bob（temp，face_id=8，限次 2）
     * 另有 dave（user，face_id=10，绑定 TOTP）：全程不参与失败用例，
     * 供「身份未定动态码」成功路径使用。 */
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
    strcpy(u.name, "dave");
    strcpy(u.role, "user");
    pin_hash("9012", salt, u.pin_hash);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled      = true;
    u.face_id      = 10;
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

static void add_expired_temp_user(void)
{
    /* carol：临时用户，有效期已过（valid_until = now - 10） */
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strcpy(u.name, "carol");
    strcpy(u.role, "temp");
    uint8_t salt[16];
    pin_hash("abcd", salt, u.pin_hash);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled     = true;
    u.face_id     = 9;
    u.face_enable = true;
    u.valid_until = (int64_t)hal_time() - 10;
    CHECK(user_add(&u) == 0);
}

int main(void)
{
    setup_store();
    fsm_reset();

    /* ---- 1.1 OK → 直接开锁（无分数，原因码即结论） ---- */
    auth_fsm_submit_face(7, FACE_RES_OK);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);
    CHECK(auth_fsm_last_reason() == FACE_RES_OK);

    /* ---- 1.2 NO_MATCH 计数（陌生人脸 -1：只计设备级，不产生用户级锁定）----
     * 1/2 次 DENY，第 3 次（face_otp_after 默认 3）转 WAIT_OTP */
    fsm_reset();
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_DENY);
    CHECK(auth_fsm_last_reason() == FACE_RES_NO_MATCH);

    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_DENY);

    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);        /* 连续 3 次未匹配 → 强制动态码 */
    CHECK(auth_fsm_pending_user()[0] == '\0');      /* 身份未定（模组未给出匹配 ID） */

    /* WAIT_OTP 中继续 NO_MATCH：继续计数，第 5 次（max_failed 默认 5）→ LOCKOUT */
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);        /* 第 4 次：未达锁定，保持等待动态码 */
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_LOCKOUT);         /* 第 5 次：设备级锁定 */

    /* LOCKOUT 期间提交结果被忽略 */
    auth_fsm_submit_face(-1, FACE_RES_OK);
    CHECK(auth_fsm_state() == FSM_LOCKOUT);

    /* 已匹配用户的 NO_MATCH 走用户级计数：连续打满触发该用户锁定（ADR-3 双轨） */
    fsm_reset();
    for(int i = 0; i < 5; i++) auth_fsm_submit_face(7, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_LOCKOUT);         /* 设备级也达 max_failed */
    {
        safe_user_t alice;
        CHECK(user_find_by_name("alice", &alice) == 0);
        CHECK(alice.lock_until > (int64_t)hal_time());   /* 用户级锁定生效 */
    }
    auth_fsm_init();
    /* alice 已被用户级锁定：动态码校验应跳过她（totp_any），bob 未绑 totp → 失败 */
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);
    {
        safe_user_t alice;
        CHECK(user_find_by_name("alice", &alice) == 0);
        char code[8] = {0};
        uint64_t now = hal_time();
        CHECK(totp_at_time(alice.totp_secret, now, 6, code, sizeof(code)) == 0);
        auth_fsm_submit_otp(code);
        CHECK(auth_fsm_state() == FSM_DENY);        /* alice 被用户级锁定 → 拒绝退出 */
    }

    /* ---- 1.3 WAIT_OTP（身份未定）+ 动态码 → 开锁（backend_verify_totp_any 路径） ----
     * 用 dave：alice 在上一段已被用户级锁定，totp_any 会正确跳过她。 */
    fsm_reset();
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);    /* 陌生人脸，同样走计数 */
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);
    {
        safe_user_t dave;
        CHECK(user_find_by_name("dave", &dave) == 0);
        char code[8] = {0};
        /* 状态机内部用 hal_time() 校验，这里必须用同一时间源生成当前窗口的码 */
        uint64_t now = hal_time();
        CHECK(totp_at_time(dave.totp_secret, now, 6, code, sizeof(code)) == 0);
        auth_fsm_submit_otp(code);
        CHECK(auth_fsm_state() == FSM_UNLOCKED);
    }

    /* ---- 1.4 WAIT_OTP 中匹配用户的 OK 帧直接放行（人脸本身即凭据） ---- */
    fsm_reset();
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_WAIT_OTP);
    auth_fsm_submit_face(7, FACE_RES_OK);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    /* ---- 1.5 LIVENESS_FAIL → 拒绝 + 计数（防伪） ---- */
    fsm_reset();
    auth_fsm_submit_face(7, FACE_RES_LIVENESS_FAIL);
    CHECK(auth_fsm_state() == FSM_DENY);
    CHECK(auth_fsm_last_reason() == FACE_RES_LIVENESS_FAIL);

    /* ---- 1.6 TIMEOUT / ERROR → 不计数不迁移 ---- */
    fsm_reset();
    auth_fsm_submit_face(7, FACE_RES_TIMEOUT);
    CHECK(auth_fsm_state() == FSM_IDLE);
    auth_fsm_submit_face(7, FACE_RES_ERROR);
    CHECK(auth_fsm_state() == FSM_IDLE);
    auth_fsm_submit_face(7, FACE_RES_OK);           /* 随后正常开锁，证明计数未被污染 */
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    /* ---- 1.7 临时用户次数用尽 → 直接删除（FR-9 v1.6，原为置停用） ---- */
    fsm_reset();
    auth_fsm_submit_face(8, FACE_RES_OK);
    CHECK(auth_fsm_state() == FSM_UNLOCKED);
    safe_user_t bob;
    CHECK(user_find_by_name("bob", &bob) == 0);
    CHECK(bob.used_count == 1);

    auth_fsm_submit_face(8, FACE_RES_OK);
    CHECK(user_find_by_name("bob", &bob) != 0);     /* 次数用尽：用户已删除 */
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    /* 删除后再刷该脸 → 陌生人处理（DENY 计数） */
    auth_fsm_submit_face(8, FACE_RES_OK);
    CHECK(auth_fsm_state() == FSM_DENY);

    /* ---- 1.8 临时用户过期 → 直接删除（FR-9 v1.6） ---- */
    fsm_reset();
    add_expired_temp_user();
    CHECK(user_find_by_name("carol", &bob) == 0);   /* 复用 bob 变量做查询 */
    auth_fsm_submit_face(9, FACE_RES_OK);
    CHECK(user_find_by_name("carol", &bob) != 0);   /* 过期：用户已删除 */
    CHECK(auth_fsm_state() == FSM_IDLE);            /* 授权失效不计设备级失败 */

    /* ---- 1.9 PIN 通道不受影响 ---- */
    fsm_reset();
    auth_fsm_submit_pin("1234");
    CHECK(auth_fsm_state() == FSM_UNLOCKED);

    fsm_reset();
    auth_fsm_submit_pin("0000");
    CHECK(auth_fsm_state() == FSM_DENY);

    /* ---- 1.10 auth_fsm_fail_streak() 只读连败计数（UI 现代化 ui1 / 规约 §5.2） ----
     * 横幅「未匹配（n/face_otp_after）」直接读它，接口必须与实际计数一致，
     * 且成功开锁与进入 LOCKOUT 后都归零。 */
    fsm_reset();
    CHECK(auth_fsm_fail_streak() == 0);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_fail_streak() == 1);
    auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_fail_streak() == 2);
    auth_fsm_submit_face(7, FACE_RES_OK);           /* 成功开锁 → 连败清零 */
    CHECK(auth_fsm_fail_streak() == 0);

    fsm_reset();
    for(int i = 0; i < 5; i++) auth_fsm_submit_face(-1, FACE_RES_NO_MATCH);
    CHECK(auth_fsm_state() == FSM_LOCKOUT);         /* 达 max_failed 进锁定 */
    CHECK(auth_fsm_fail_streak() == 0);             /* 进入锁定时同时清零 */
    CHECK(auth_fsm_lock_remaining() > 0);

    /* TIMEOUT / ERROR 不计数（与 1.6 同源，这里专门锁住 fail_streak 不被污染） */
    fsm_reset();
    auth_fsm_submit_face(7, FACE_RES_TIMEOUT);
    auth_fsm_submit_face(7, FACE_RES_ERROR);
    CHECK(auth_fsm_fail_streak() == 0);

    TEST_RESULT();
}

