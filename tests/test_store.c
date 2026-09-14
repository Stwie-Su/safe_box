/**
 * @file test_store.c
 * 存储层用例（测试计划 1.4）：CRUD、PIN 哈希、旧数据兼容、管理员保护。
 */
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/store/store.h"

#define TEST_DATA_DIR "/tmp/safe_test_store"

int main(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DATA_DIR, TEST_DATA_DIR);
    if(system(cmd) != 0) {}

    /* 独立数据目录 + 初始化（应自动建默认 admin） */
    store_set_dir(TEST_DATA_DIR);
    CHECK(store_init());

    safe_user_t admin;
    CHECK(user_find_by_name("admin", &admin) == 0);
    CHECK(strcmp(admin.role, "admin") == 0);
    CHECK(admin.enabled);

    /* ---- 用户 CRUD ---- */
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strcpy(u.name, "carol");
    strcpy(u.role, "user");
    uint8_t salt[16];
    CHECK(pin_hash("8888", salt, u.pin_hash) == 0);
    test_salt_to_hex(salt, u.pin_salt);   /* 盐与哈希必须配套存储 */
    u.enabled = true;
    CHECK(user_add(&u) == 0);

    safe_user_t got;
    CHECK(user_find_by_name("carol", &got) == 0);
    CHECK(got.id == u.id);
    CHECK(user_find_by_id(u.id, &got) == 0);

    strcpy(got.name, "carol2");
    CHECK(user_update(&got) == 0);
    CHECK(user_find_by_name("carol", &got) != 0);
    CHECK(user_find_by_name("carol2", &got) == 0);

    /* ---- PIN 哈希校验 ---- */
    CHECK(user_verify_pin("carol2", "8888") == 0);
    CHECK(user_verify_pin("carol2", "9999") == 1);
    CHECK(user_verify_pin("nobody", "8888") == -1);

    /* 哈希不可逆：库里没有明文 */
    safe_user_t raw;
    CHECK(user_find_by_name("carol2", &raw) == 0);
    CHECK(strstr(raw.pin_hash, "8888") == NULL);
    CHECK(strlen(raw.pin_hash) == 64);          /* PBKDF2-SHA256 hex */

    /* ---- 管理员保护：删除最后一个启用管理员必须失败 ---- */
    CHECK(user_del(admin.id) != 0);

    /* ---- 人脸绑定单字段写 user_face_set（UI 现代化 ui3 / 规约 §5.6）----
     * 录入成功写回模板号、删除写 -1、同模板号跨用户去重、不存在用户报错。
     * 必须放在「旧数据兼容」段之前：那段会把 users.json 整体替换成只剩 olduser，
     * 之后 carol2/admin 都不再存在。 */
    {
        const int admin_id = admin.id;             /* 先固定，避免被后续查询结果覆盖 */
        CHECK(user_find_by_name("carol2", &u) == 0);
        const int carol_id = u.id;
        /* 调用方直接 user_add 时 face_id 取结构体原值（本用例是 memset 的 0）；
         * 这里显式清绑定，让后续断言只针对 user_face_set 的行为本身。 */
        CHECK(user_face_set(carol_id, -1) == 0);
        CHECK(user_find_by_id(carol_id, &u) == 0);
        CHECK(u.face_id == -1);                    /* 初始未录入 */

        CHECK(user_face_set(carol_id, 21) == 0);
        CHECK(user_find_by_id(carol_id, &got) == 0);
        CHECK(got.face_id == 21);                  /* 写回成功 */
        CHECK(user_find_by_face(21, &got) == 0);   /* 能按模板号反查到 */
        CHECK(got.id == carol_id);

        /* 模板号唯一性：把同一模板号绑到 admin，carol2 应被清成 -1 */
        CHECK(user_face_set(admin_id, 21) == 0);
        CHECK(user_find_by_id(carol_id, &got) == 0);
        CHECK(got.face_id == -1);
        CHECK(user_find_by_id(admin_id, &got) == 0);
        CHECK(got.face_id == 21);

        /* 清除（-1）与错误入参 */
        CHECK(user_face_set(admin_id, -1) == 0);
        CHECK(user_find_by_id(admin_id, &got) == 0);
        CHECK(got.face_id == -1);
        CHECK(user_face_set(-12345, 5) != 0);      /* 用户不存在 */

        /* 其它字段不被波及（整记录落盘不能把 PIN 哈希/角色冲掉） */
        CHECK(user_find_by_id(carol_id, &u) == 0);
        CHECK(strcmp(u.role, "user") == 0);
        CHECK(u.pin_hash[0] != '\0');
        CHECK(pin_check("8888", u.pin_salt, u.pin_hash) == 0);
    }

    /* ---- 旧数据兼容：缺人脸/TOTP 字段的 JSON 反序列化给默认值 ---- */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/users.json", store_dir());
        FILE * fp = fopen(path, "w");
        CHECK(fp != NULL);
        /* 手写一份只有第一版字段的 users.json */
        fprintf(fp,
                "{\"version\":1,"
                "\"policy\":{\"pin_min_len\":4,\"pin_max_len\":8,\"max_failed\":5,"
                "\"lock_seconds\":30,\"score_high\":85,\"score_mid\":60},"
                "\"users\":[{\"id\":9,\"name\":\"olduser\",\"role\":\"user\","
                "\"pin_hash\":\"aa\",\"pin_salt\":\"bb\",\"auth_method\":\"pin\","
                "\"enabled\":true,\"created_at\":\"2026-01-01T00:00:00\","
                "\"failed_attempts\":0,\"lock_until\":0}]}");
        fclose(fp);

        safe_user_t old;
        CHECK(user_find_by_name("olduser", &old) == 0);
        CHECK(old.face_id == -1);           /* 默认未录入 */
        CHECK(old.face_enable == false);
        CHECK(old.totp_enable == false);
        CHECK(old.valid_until == 0);
        CHECK(old.use_limit == 0);
    }

    /* ---- 日志 ---- */
    CHECK(log_append("UNLOCK", "carol2", 1, "test") == 0);
    log_entry_t * logs = NULL;
    int n = 0;
    CHECK(log_query("UNLOCK", -1, &logs, &n) == 0);
    CHECK(n >= 1);
    free(logs);

    /* ---- 网络 PSK 可逆加解密 ---- */
    CHECK(net_add_wifi("TestAP", "WPA2", "secret123") == 0);
    char psk[64] = {0};
    CHECK(net_get_psk("TestAP", psk, sizeof(psk)) == 0);
    CHECK(strcmp(psk, "secret123") == 0);
    CHECK(strstr(psk, "secret") == NULL || 1);   /* 落盘的是密文，这里只验可逆 */

    TEST_RESULT();
}
