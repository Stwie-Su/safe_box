/* tests/test_store_p0.c — P0 回归门禁（QA-01 / QA-02）
 *
 * 背景：这两个缺陷都是 ASan 实锤的内存破坏，且都发生在**正常业务分支**上：
 *   QA-01 log_query：日志末尾留半行（掉电 / kill -9 的典型残留）时，
 *        分配按 '\n' 个数、写入按 strtok 段数 → 越界写 sizeof(log_entry_t)。
 *        现实路径 = 设备掉电重启后打开「日志」页。
 *   QA-02 user_del：删除唯一启用管理员时，先 user_list_free() 再读
 *        us[target_idx].name 写审计日志 → use-after-free。
 * 这两个 bug 在 ASan 下会直接 abort，因此「本测试能跑完」本身就是判据；
 * 其余断言用于确认**行为正确**（而不只是没崩）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/store/store.h"

static void wr(const char *dir, const char *name, const char *content)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror("fopen"); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static const char *USERS_ONE_ADMIN =
"{\n  \"version\": 1,\n  \"policy\": {\"pin_min_len\": 4, \"pin_max_len\": 8, "
"\"max_failed\": 5, \"lock_seconds\": 30, \"face_otp_after\": 3, "
"\"face_verify_timeout_s\": 10, \"virtual_pin_enable\": true},\n  \"users\": [\n"
"    {\"id\":1,\"name\":\"admin\",\"role\":\"admin\",\"pin_hash\":\"aa\",\"pin_salt\":\"bb\","
"\"auth_method\":\"pin\",\"enabled\":true,\"created_at\":\"2026-01-01T00:00:00\",\"failed_attempts\":0,\"lock_until\":0,"
"\"face_id\":-1,\"face_enable\":false,\"totp_enable\":false,\"totp_secret\":\"\",\"last_otp_counter\":0,\"valid_until\":0,\"use_limit\":0,\"used_count\":0}\n"
"  ]\n}\n";

int main(void)
{
    int rc = 0;

    /* ---------- QA-01：末尾半行不得越界 ---------- */
    system("rm -rf /tmp/p0_log; mkdir -p /tmp/p0_log");
    store_set_dir("/tmp/p0_log");
    wr("/tmp/p0_log", "safe.log",
       "{\"ts\":\"t\",\"evt\":\"a\",\"user\":\"u\",\"res\":1,\"detail\":\"x\"}\n"
       "{\"ts\":\"t\",\"evt\":\"b\",\"user\":\"u\",\"res\":1,\"detail\":\"x\"}\n"
       "{\"ts\":\"t\",\"evt\":\"c\",\"user\":\"u\",\"res\":1,\"detail\":\"x\"}\n"
       "{\"ts\":\"t\",\"evt\":\"HALF");          /* 无 \n 结尾的半行 */
    {
        log_entry_t *out = NULL; int n = 0;
        int r = log_query(NULL, -1, &out, &n);
        printf("[QA-01] log_query r=%d n=%d （3 行完整 + 1 行半行）\n", r, n);
        if (r != 0 || n != 3) {
            printf("        FAIL：期望 r=0 n=3（半行应被丢弃）\n");
            rc = 1;
        } else {
            printf("        PASS：半行已丢弃且无越界（越界时 ASan 会 abort）\n");
        }
        free(out);
    }

    /* ---------- QA-02：删除唯一启用管理员，不得 UAF ---------- */
    system("rm -rf /tmp/p0_uaf; mkdir -p /tmp/p0_uaf");
    store_set_dir("/tmp/p0_uaf");
    wr("/tmp/p0_uaf", "users.json", USERS_ONE_ADMIN);
    {
        int r = user_del(1);
        printf("[QA-02] user_del(唯一启用 admin) = %d （期望 -1）\n", r);
        if (r != -1) { printf("        FAIL：应当拒绝删除\n"); rc = 1; }
    }
    {
        log_entry_t *out = NULL; int n = 0;
        log_query("user_del", -1, &out, &n);
        int ok = 0;
        for (int i = 0; i < n; i++) {
            if (out[i].user[0] != '\0' && strcmp(out[i].user, "admin") == 0) ok = 1;
        }
        printf("[QA-02] 审计日志用户名 = %s （期望 admin）\n",
               (n > 0 ? out[0].user : "(无日志)"));
        if (!ok) {
            printf("        FAIL：用户名未正确写入（UAF 会读到已释放内存）\n");
            rc = 1;
        } else {
            printf("        PASS：名字在 free 之前已拷贝，审计正确\n");
        }
        free(out);
    }

    printf(rc ? "=== P0 REGRESSION: FAIL ===\n" : "=== P0 REGRESSION: PASS ===\n");
    return rc;
}
