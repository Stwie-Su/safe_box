/**
 * @file test_worker.c
 * worker（单后台线程 + 主线程 done 派发）行为契约单测（QA 补缺，2026-10-06）。
 *
 * 锁定的不变式（违反任意一条，在真实业务里就是 UI 崩溃或数据丢失）：
 *   1) fn 在**后台线程**执行，且严格按提交顺序串行（单 worker）；
 *   2) done 在**调用 worker_poll() 的线程**执行（LVGL 只能在主线程操作，
 *      这条是整个多线程架构的地基）；
 *   3) 未 init 时 worker_poll / worker_shutdown 为空操作，不崩不挂；
 *   4) worker_shutdown **不丢在途作业**（pending 跑完 + done 补齐后才返回）；
 *   5) shutdown 幂等；shutdown 之后可重新 init（进程内允许一轮生命周期重启）。
 *
 * 只依赖 pthread，不需要 LVGL / 网络 / 文件系统。等待用有界轮询（上限约 2s），
 * 机器负载高时不会误判为失败。
 */
/* 顶层是 -std=c99（__STRICT_ANSI__），POSIX 接口默认不可见；
 * usleep 在 POSIX-2008 已被废弃，须靠 _DEFAULT_SOURCE 而非 _POSIX_C_SOURCE 暴露。 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_util.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/support/worker.h"

#define JOB_N 50

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_fn_seq[JOB_N];
static int g_fn_n;
static int g_done_seq[JOB_N];
static int g_done_n;
static int g_ff_n;               /* fire-and-forget 计数 */

static pthread_t g_fn_tid;       /* 记录 fn 实际运行在哪个线程 */
static int       g_fn_tid_seen;
static pthread_t g_done_tid;     /* 记录 done 实际运行在哪个线程 */
static int       g_done_tid_seen;

typedef struct { int idx; } job_arg_t;

/* 后台段：只记录，不碰任何主线程资源 */
static void job_fn(void * arg)
{
    job_arg_t * a = (job_arg_t *)arg;
    pthread_mutex_lock(&g_lock);
    if (g_fn_n < JOB_N) g_fn_seq[g_fn_n++] = a->idx;
    if (!g_fn_tid_seen) { g_fn_tid = pthread_self(); g_fn_tid_seen = 1; }
    pthread_mutex_unlock(&g_lock);
}

/* 主线程段（worker_poll 内）：按所有权契约释放 arg */
static void job_done(void * arg)
{
    job_arg_t * a = (job_arg_t *)arg;
    pthread_mutex_lock(&g_lock);
    if (g_done_n < JOB_N) g_done_seq[g_done_n++] = a->idx;
    if (!g_done_tid_seen) { g_done_tid = pthread_self(); g_done_tid_seen = 1; }
    pthread_mutex_unlock(&g_lock);
    free(a);
}

/* fire-and-forget：arg 由框架在 done == NULL 时释放 */
static void ff_fn(void * arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    g_ff_n++;
    pthread_mutex_unlock(&g_lock);
}

static void snapshot(int * fn, int * dn, int * ff)
{
    pthread_mutex_lock(&g_lock);
    *fn = g_fn_n; *dn = g_done_n; *ff = g_ff_n;
    pthread_mutex_unlock(&g_lock);
}

/* 主线程泵：poll + 有界等待（4000 × 500us ≈ 2s 上限） */
static void pump_until(int want_fn, int want_done)
{
    for (int i = 0; i < 4000; i++) {
        worker_poll();
        int fn = 0, dn = 0, ff = 0;
        snapshot(&fn, &dn, &ff);
        if (fn >= want_fn && dn >= want_done) return;
        usleep(500);
    }
}

static void reset_counters(void)
{
    pthread_mutex_lock(&g_lock);
    g_fn_n = 0; g_done_n = 0; g_ff_n = 0;
    pthread_mutex_unlock(&g_lock);
}

static void post_range(int from, int count)
{
    for (int i = 0; i < count; i++) {
        job_arg_t * a = (job_arg_t *)malloc(sizeof(*a));
        if (!a) return;
        a->idx = from + i;
        worker_post(job_fn, a, job_done);
    }
}

int main(void)
{
    /* ---- 1. 未 init：poll / shutdown 都必须是空操作 ---- */
    {
        worker_poll();       /* 未 init：done 队列为空，不得崩溃 */
        worker_shutdown();   /* 未 init：幂等返回，不得挂住 */
        int fn = 0, dn = 0, ff = 0;
        snapshot(&fn, &dn, &ff);
        CHECK(fn == 0 && dn == 0 && ff == 0);
    }

    /* ---- 2. init 幂等（重复调用不得起第二个线程） ---- */
    worker_init();
    worker_init();

    /* ---- 3. 顺序性 + 线程归属 ---- */
    reset_counters();
    post_range(0, JOB_N);
    pump_until(JOB_N, JOB_N);

    {
        int fn = 0, dn = 0, ff = 0;
        snapshot(&fn, &dn, &ff);
        CHECK(fn == JOB_N);
        CHECK(dn == JOB_N);

        /* fn 严格按提交顺序（单 worker 串行执行） */
        int ordered_fn = 1, ordered_done = 1;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < JOB_N; i++) {
            if (g_fn_seq[i] != i)   ordered_fn = 0;
            if (g_done_seq[i] != i) ordered_done = 0;
        }
        pthread_mutex_unlock(&g_lock);
        CHECK(ordered_fn == 1);
        CHECK(ordered_done == 1);

        /* fn 在**后台**线程：绝不能是主线程 */
        CHECK(g_fn_tid_seen == 1);
        CHECK(pthread_equal(g_fn_tid, pthread_self()) == 0);
        /* done 在**主线程**（调用 worker_poll 的线程）：LVGL 操作安全的前提 */
        CHECK(g_done_tid_seen == 1);
        CHECK(pthread_equal(g_done_tid, pthread_self()) != 0);
    }

    /* ---- 4. fire-and-forget：done == NULL 时框架负责释放 arg ---- */
    {
        reset_counters();
        for (int i = 0; i < 5; i++) {
            job_arg_t * a = (job_arg_t *)malloc(sizeof(*a));
            if (a) { a->idx = 100 + i; worker_post(ff_fn, a, NULL); }
        }
        worker_post(ff_fn, NULL, NULL);   /* arg = NULL：free(NULL) 合法 */

        int fn = 0, dn = 0, ff = 0;
        for (int i = 0; i < 4000; i++) {
            worker_poll();
            snapshot(&fn, &dn, &ff);
            if (ff >= 6) break;
            usleep(500);
        }
        CHECK(ff == 6);
        /* fire-and-forget 不产生 done 回调，也不占用顺序计数 */
        CHECK(dn == 0);
    }

    /* ---- 5. shutdown：在途作业不丢 ---- */
    {
        reset_counters();
        post_range(0, 5);
        worker_shutdown();   /* 必须先把 5 条跑完、并把 done 补齐后才返回 */

        int fn = 0, dn = 0, ff = 0;
        snapshot(&fn, &dn, &ff);
        CHECK(fn == 5);
        CHECK(dn == 5);
    }

    /* ---- 6. shutdown 幂等（二次调用不得挂、不得二次 join） ---- */
    worker_shutdown();
    worker_shutdown();

    /* ---- 7. shutdown 后可重新 init 并继续工作（g_stop 必须被复位） ---- */
    {
        reset_counters();
        worker_init();
        post_range(0, 3);
        pump_until(3, 3);
        int fn = 0, dn = 0, ff = 0;
        snapshot(&fn, &dn, &ff);
        CHECK(fn == 3);
        CHECK(dn == 3);
        worker_shutdown();
    }

    TEST_RESULT();
}
