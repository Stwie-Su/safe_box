/**
 * @file store_cache.c
 * 只读用户表快照缓存实现（双缓冲：worker 写 inactive，主线程读 active）。
 *
 * 硬约束：不申请堆内存、不碰文件、不取时间 —— 全部静态数组。
 * 读侧只做「取指针 → memcpy」，因此不会阻塞，也不会与写侧形成死锁。
 */
#include "core/store/store_cache.h"

#include <string.h>

/* 双缓冲：两块定长表 + 各自的有效条数 */
static safe_user_t  g_buf[2][STORE_CACHE_USER_MAX];
static int          g_count[2];

/* 当前有效块下标（0/1）。volatile：写侧切换后读侧必须立刻看到新值。 */
static volatile int g_active = 0;

/* 快照版本号：每次发布 +1。读侧用它判断「这份数据有多旧 / 是否从未发布」。 */
static volatile uint32_t g_stamp = 0;

void store_cache_publish_users(const safe_user_t *list, int count)
{
    if (list == NULL) count = 0;    /* 传 NULL 即「发布一份空表」，count 不生效 */
    if (count < 0) count = 0;
    if (count > STORE_CACHE_USER_MAX) count = STORE_CACHE_USER_MAX;

    /* ★ 写 inactive 块：读侧此时仍在读 active 块，不会被这次写入打扰。 */
    const int w = g_active ^ 1;

    if (list != NULL && count > 0) {
        memcpy(g_buf[w], list, (size_t)count * sizeof(safe_user_t));
    }
    g_count[w] = count;

    /* 先填数据、后切指针（顺序不能反：反了就有窗口读到半张表）。 */
    g_stamp = g_stamp + 1u;
    g_active = w;
}

int store_cache_user_count(int *count, uint32_t *stamp)
{
    const int a = g_active;
    if (count != NULL) *count = g_count[a];
    if (stamp != NULL) *stamp = g_stamp;
    return 0;
}

int store_cache_find_by_id(int id, safe_user_t *out)
{
    const int a = g_active;
    for (int i = 0; i < g_count[a]; i++) {
        if (g_buf[a][i].id != id) continue;
        if (out != NULL) *out = g_buf[a][i];
        return 0;
    }
    return -1;
}
