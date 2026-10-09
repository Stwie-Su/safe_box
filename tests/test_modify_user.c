/**
 * @file test_modify_user.c
 * 「修改信息」写通道 astore_modify_user 用例（需求 v1.11 / FR-9 修改入口）。
 *
 * 锁定的不变式（每条背后都是一个真实缺陷或一条需求边界）：
 *   1) 改名 + 改 PIN + 改角色 + 改临时限制是**同一次提交**——要么全改，要么
 *      一个字段都不改（历史缺陷：先改名落盘再验旧 PIN，旧 PIN 错时名字已改而
 *      UI 报「旧 PIN 错误」，界面与存储不一致）；
 *   2) 末位启用管理员不得降级为非管理员（否则敏感操作永久锁死），且被拒后
 *      该用户**任何字段**都不落盘；
 *   3) 次数上限 ≤ 已用次数一律拒绝——否则该用户下次认证即命中「次数已用尽」
 *      被删除（含人脸凭据），改配置不该等价于删号；
 *   4) 角色由临时改为非临时时，有效期 / 上限 / 已用次数一并清零（不留脏数据）；
 *   5) 非法角色 / 重名 / 旧 PIN 错误各有独立结果码，且同样零落盘。
 *
 * 不依赖 LVGL；数据落在独立临时目录；后台走真实 worker 线程（与本模块在
 * 产品里的运行形态一致），done 由主线程 worker_poll 派发。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/store/store.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"

#define TEST_DATA_DIR "/tmp/safe_test_modify"

/* ---------------- 异步结果等待 ---------------- */
static volatile int g_result;
static volatile int g_done;

static void on_modify(int result)
{
    g_result = result;
    g_done   = 1;
}

/* 提交一个修改作业并等结果。有界轮询（约 2s 上限），机器负载高时不会误判失败。 */
static int modify_wait(const modify_user_req_t * req)
{
    g_done   = 0;
    g_result = -999;
    astore_modify_user(req, on_modify);
    for (int i = 0; i < 2000 && !g_done; i++) {
        worker_poll();
        usleep(1000);
    }
    CHECK(g_done == 1);            /* 超时即失败：作业没能完成 */
    return g_result;
}

/* ---------------- 工具 ---------------- */
static void add_user(int id, const char * name, const char * role, const char * pin,
                     int64_t valid_until, int use_limit, int used_count)
{
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = id;
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
    CHECK(user_add(&u) == 0);
}

static int find_user(int id, safe_user_t * out)
{
    safe_user_t * us = NULL;
    int n = 0, rc = -1;
    if (user_load_all(&us, &n) == 0) {
        for (int i = 0; i < n; i++)
            if (us[i].id == id) { *out = us[i]; rc = 0; break; }
    }
    user_list_free(us);
    return rc;
}

static int find_admin_id(void)
{
    safe_user_t * us = NULL;
    int n = 0, id = -1;
    if (user_load_all(&us, &n) == 0) {
        for (int i = 0; i < n; i++)
            if (strcmp(us[i].role, "admin") == 0 && us[i].enabled) { id = us[i].id; break; }
    }
    user_list_free(us);
    return id;
}

static void req_init(modify_user_req_t * r, int id)
{
    memset(r, 0, sizeof(*r));
    r->id = id;
}

/* ---------------- 用例 ---------------- */
int main(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DATA_DIR, TEST_DATA_DIR);
    if (system(cmd) != 0) { /* 失败会在 store_init 里暴露 */ }
    store_set_dir(TEST_DATA_DIR);
    CHECK(store_init() == true);

    worker_init();

    /* 用户集：store_init 自带的默认管理员 + 一个普通用户 + 两个临时用户。
     * ★ 全程不再自建管理员——「末位管理员」用例要的就是默认管理员是唯一一个。 */
    add_user(10, "bob",  "user", "1234", 0, 0, 0);
    add_user(11, "tmp1", "temp", "1234", 0, 5, 2);
    add_user(12, "tmp2", "temp", "1234", 0, 0, 3);     /* 不限次、已用 3 次 */

    /* ---- 1. 普通用户 → 临时用户，并设有效期与次数上限 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 10);
        strncpy(r.role, "temp", sizeof(r.role) - 1);
        r.valid_until = (int64_t)time(NULL) + 3 * 86400;
        r.use_limit   = 10;
        CHECK(modify_wait(&r) == MODIFY_OK);

        safe_user_t u;
        CHECK(find_user(10, &u) == 0);
        CHECK(strcmp(u.role, "temp") == 0);
        CHECK(u.use_limit == 10);
        CHECK(u.valid_until > 0);
        CHECK(u.used_count == 0);
    }

    /* ---- 2. 临时用户 → 普通用户：三项限制必须清零（边界 ③） ---- */
    {
        modify_user_req_t r;
        req_init(&r, 11);
        strncpy(r.role, "user", sizeof(r.role) - 1);
        CHECK(modify_wait(&r) == MODIFY_OK);

        safe_user_t u;
        CHECK(find_user(11, &u) == 0);
        CHECK(strcmp(u.role, "user") == 0);
        CHECK(u.valid_until == 0);
        CHECK(u.use_limit   == 0);
        CHECK(u.used_count  == 0);      /* 原先是 2，改角色后必须清掉 */
    }

    /* ---- 3. 末位启用管理员不得降级（边界 ①），且拒绝后零落盘 ---- */
    {
        const int admin_id = find_admin_id();
        CHECK(admin_id >= 0);

        modify_user_req_t r;
        req_init(&r, admin_id);
        strncpy(r.role, "user", sizeof(r.role) - 1);
        CHECK(modify_wait(&r) == MODIFY_ERR_LAST_ADMIN);

        safe_user_t u;
        CHECK(find_user(admin_id, &u) == 0);
        CHECK(strcmp(u.role, "admin") == 0);   /* ★ 被拒 → 角色原封不动 */
    }

    /* ---- 4. 次数上限 ≤ 已用次数一律拒绝（边界 ②），且零落盘 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 12);
        strncpy(r.role, "temp", sizeof(r.role) - 1);   /* 角色未变，只改上限 */
        r.use_limit = 3;                                /* used_count = 3 → 3 >= 3 */
        CHECK(modify_wait(&r) == MODIFY_ERR_LIMIT);

        safe_user_t u;
        CHECK(find_user(12, &u) == 0);
        CHECK(u.use_limit  == 0);       /* ★ 被拒 → 没写成 3 */
        CHECK(u.used_count == 3);
    }

    /* ---- 5. 改名 + 改 PIN 一次提交成功 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 12);
        strncpy(r.new_name, "tmp2new", sizeof(r.new_name) - 1);
        strncpy(r.old_pin,  "1234", sizeof(r.old_pin) - 1);
        strncpy(r.new_pin,  "5678", sizeof(r.new_pin) - 1);
        CHECK(modify_wait(&r) == MODIFY_OK);

        safe_user_t u;
        CHECK(find_user(12, &u) == 0);
        CHECK(strcmp(u.name, "tmp2new") == 0);
        CHECK(user_verify_pin("tmp2new", "5678") == 0);   /* 新 PIN 生效 */
        CHECK(user_verify_pin("tmp2new", "1234") == 1);   /* 旧 PIN 失效 */
    }

    /* ---- 6. 旧 PIN 错误：整单作废，名字**不能**已经改掉（历史缺陷回归闸门） ---- */
    {
        modify_user_req_t r;
        req_init(&r, 12);
        strncpy(r.new_name, "hacked", sizeof(r.new_name) - 1);
        strncpy(r.old_pin,  "0000", sizeof(r.old_pin) - 1);   /* 错 */
        strncpy(r.new_pin,  "9999", sizeof(r.new_pin) - 1);
        CHECK(modify_wait(&r) == MODIFY_ERR_OLDPIN);

        safe_user_t u;
        CHECK(find_user(12, &u) == 0);
        CHECK(strcmp(u.name, "tmp2new") == 0);   /* ★ 原样：没有部分提交 */
        CHECK(user_verify_pin("tmp2new", "5678") == 0);
    }

    /* ---- 7. 重名拒绝，且零落盘 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 12);
        strncpy(r.new_name, "bob", sizeof(r.new_name) - 1);   /* bob 是 10 号 */
        CHECK(modify_wait(&r) == MODIFY_ERR_DUPNAME);

        safe_user_t u;
        CHECK(find_user(12, &u) == 0);
        CHECK(strcmp(u.name, "tmp2new") == 0);
    }

    /* ---- 8. 非法角色拒绝，且零落盘 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 10);
        strncpy(r.role, "superuser", sizeof(r.role) - 1);
        CHECK(modify_wait(&r) == MODIFY_ERR_ROLE);

        safe_user_t u;
        CHECK(find_user(10, &u) == 0);
        CHECK(strcmp(u.role, "temp") == 0);
    }

    /* ---- 9. 无此用户 ---- */
    {
        modify_user_req_t r;
        req_init(&r, 9999);
        strncpy(r.role, "user", sizeof(r.role) - 1);
        CHECK(modify_wait(&r) == MODIFY_ERR_STORE);
    }

    worker_shutdown();
    TEST_RESULT();
}
