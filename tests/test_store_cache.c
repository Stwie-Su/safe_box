/**
 * @file test_store_cache.c
 * 只读用户表快照缓存（store_cache）单元测试。
 *
 * 锁定三条语义：
 *   1. 双缓冲：写侧填 inactive、填完切指针 —— 读侧**永远读不到半张表**
 *      （表现为：读出来的 `safe_user_t` 副本在后续发布之后仍然自洽）；
 *   2. stamp 单调 +1，stamp==0 表示「从未发布」；
 *   3. 读接口不落盘、不阻塞、NULL 安全；写接口对越界 count 做钳位。
 */
#include "test_util.h"

#include <string.h>

#include "core/store/store_cache.h"

static void mk_user(safe_user_t *u, int id, const char *name)
{
    memset(u, 0, sizeof(*u));
    u->id        = id;
    u->face_id   = -1;
    u->enabled   = true;
    u->valid_until = 0;
    snprintf(u->name, sizeof(u->name), "%s", name);
    snprintf(u->role, sizeof(u->role), "user");
}

int main(void)
{
    /* ---- 1. 初始状态：从未发布 ---- */
    {
        int n = -1;
        uint32_t stamp = 999;
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == 0);
        CHECK(stamp == 0);                    /* ★ stamp==0 = 从未发布过 */
        CHECK(store_cache_find_by_id(1, NULL) == -1);
    }

    /* ---- 2. 发布一份 3 用户快照 ---- */
    {
        safe_user_t list[3];
        int n = 0;
        uint32_t stamp = 0;
        mk_user(&list[0], 1, "alice");
        mk_user(&list[1], 2, "bob");
        mk_user(&list[2], 3, "carol");

        store_cache_publish_users(list, 3);
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == 3);
        CHECK(stamp == 1);

        safe_user_t got;
        memset(&got, 0, sizeof(got));
        CHECK(store_cache_find_by_id(1, &got) == 0);
        CHECK(got.id == 1);
        CHECK(strcmp(got.name, "alice") == 0);
        CHECK(store_cache_find_by_id(3, &got) == 0);
        CHECK(strcmp(got.name, "carol") == 0);
        CHECK(store_cache_find_by_id(99, &got) == -1);
    }

    /* ---- 3. 再发布一份（只有 2 人）→ 快照整体替换，旧条目消失 ---- */
    {
        safe_user_t list[2];
        int n = 0;
        uint32_t stamp = 0;
        mk_user(&list[0], 10, "dave");
        mk_user(&list[1], 11, "erin");

        store_cache_publish_users(list, 2);
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == 2);
        CHECK(stamp == 2);
        CHECK(store_cache_find_by_id(10, NULL) == 0);
        CHECK(store_cache_find_by_id(1, NULL) == -1);   /* 上一版的 alice 已不在 */
    }

    /* ---- 4. 双缓冲的关键性质：读出的副本不受后续发布影响 ---- */
    {
        safe_user_t a[1], b[1];
        safe_user_t snap;
        memset(&snap, 0, sizeof(snap));

        mk_user(&a[0], 7, "before");
        store_cache_publish_users(a, 1);
        CHECK(store_cache_find_by_id(7, &snap) == 0);   /* 拷一份出来 */

        mk_user(&b[0], 7, "after");
        store_cache_publish_users(b, 1);

        /* 之后重新查，必须看到新值 */
        safe_user_t now;
        memset(&now, 0, sizeof(now));
        CHECK(store_cache_find_by_id(7, &now) == 0);
        CHECK(strcmp(now.name, "after") == 0);
        /* 而先前拷出来的那份仍自洽（memcpy 副本，不是指向内部缓冲的指针） */
        CHECK(snap.id == 7);
        CHECK(strcmp(snap.name, "before") == 0);
    }

    /* ---- 5. 写侧边界：NULL / count 越界 ---- */
    {
        int n = -1;
        uint32_t stamp = 0;
        store_cache_publish_users(NULL, 5);
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == 0);                     /* list 为 NULL → 空表 */

        safe_user_t big[STORE_CACHE_USER_MAX + 8];
        for (int i = 0; i < STORE_CACHE_USER_MAX + 8; i++) mk_user(&big[i], 100 + i, "u");
        store_cache_publish_users(big, STORE_CACHE_USER_MAX + 8);
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == STORE_CACHE_USER_MAX);  /* 超过容量 → 钳位，不越界写 */
        CHECK(store_cache_find_by_id(100, NULL) == 0);
        CHECK(store_cache_find_by_id(100 + STORE_CACHE_USER_MAX, NULL) == -1);

        store_cache_publish_users(big, -3);   /* 负数 → 视为 0 */
        CHECK(store_cache_user_count(&n, &stamp) == 0);
        CHECK(n == 0);
    }

    /* ---- 6. 读侧 NULL 安全 ---- */
    {
        CHECK(store_cache_user_count(NULL, NULL) == 0);
        CHECK(store_cache_find_by_id(1, NULL) == -1);
    }

    TEST_RESULT();
}
