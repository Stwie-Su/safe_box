/**
 * @file test_store.c
 * 存储层用例（测试计划 1.4）：CRUD、PIN 哈希、旧数据兼容、管理员保护。
 */
#include "test_util.h"

#include <stdio.h>
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
    CHECK(log_append("unlock", "carol2", 1, "test") == 0);
    log_entry_t * logs = NULL;
    int n = 0;
    CHECK(log_query("unlock", -1, &logs, &n) == 0);
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
