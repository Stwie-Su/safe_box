/**
 * @file worker.c
 * 通用异步后台 worker 实现（DESIGN.md §9）。
 *
 * 两个 FIFO 链表队列：
 *  - pending 队列：存储待处理任务。UI 线程往里投递，worker 线程从里取出并执行。
 *  - done 队列   ：存储已完成任务。worker 线程执行完后放入，主线程 worker_poll() 取出并回调。
 *
 * 核心机制：
 *  全程仅需一个互斥锁 + 一个条件变量；
 *  所有的 done 回调函数均保证在调用 worker_poll() 的主线程中执行（确保 LVGL 等 UI 操作安全）。
 */
#include "worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>

/* --- 作业节点数据结构 --- */
typedef struct job {
    worker_fn_t   fn;     /* 后台耗时任务函数指针 */
    void        * arg;    /* 自定义数据 */
    worker_done_t done;   /* 完成回调函数指针：ui回显*/
    struct job  * next;   /* 链表下一个节点的指针 */
} job_t;

/* --- 全局并发控制与队列状态 --- */
static pthread_t       g_thread;                        /* 后台工作线程句柄 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* 保护两个队列的全局互斥锁 */
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;  /* 唤醒 worker 线程的条件变量 */

/* 待处理任务队列 (Pending Queue) 的头尾指针 */
static job_t * g_pending_head, * g_pending_tail;
/* 已完成任务队列 (Done Queue) 的头尾指针 */
static job_t * g_done_head,   * g_done_tail;

/* 线程退出标志位 (true 时通知后台线程退出循环) */
static bool    g_stop = false;

/**
 * @brief 后台工作线程的主循环函数
 * 
 * @param p 线程传入参数（未指定，强制转 void 忽略警告）
 * @return void* 线程返回值
 */
static void * worker_main(void * p)
{
    (void)p;
    for (;;) {
        /* ================= 阶段 1：从 Pending 队列安全提取任务 ================= */
        pthread_mutex_lock(&g_lock);

        /* 
         * 当待处理队列为空，且系统未发出停止信号时，线程进入阻塞休眠。
         * 注意：pthread_cond_wait 会在休眠前自动释放 g_lock 锁，
         * 被唤醒后再重新抢占并持有 g_lock 锁。必须使用 while 循环防止伪唤醒（Spurious Wakeup）。
         */
        while (!g_pending_head && !g_stop) {
            pthread_cond_wait(&g_cond, &g_lock);
        }

        /* 如果收到退出指令且队列已清空，解锁并退出线程 */
        if (!g_pending_head && g_stop) {
            pthread_mutex_unlock(&g_lock);
            break;
        }

        /* 从 pending 链表头部摘除一个待执行任务 (FIFO 出队) */
        job_t * j = g_pending_head;
        g_pending_head = j->next;
        if (!g_pending_head) g_pending_tail = NULL; /* 如果队列取空了，同步置空尾指针 */

        pthread_mutex_unlock(&g_lock); // 【关键】马上解锁！绝不可拿着锁去执行耗时任务

        /* ================= 阶段 2：在后台线程中执行耗时业务 ================= */
        j->fn(j->arg);              /* 阻塞任务（如网络连接、哈希计算、文件 I/O）在此处执行 */

        /* ================= 阶段 3：将完成的任务塞入 Done 队列 ================= */
        pthread_mutex_lock(&g_lock); // 再次加锁以安全修改 done 队列

        j->next = NULL;
        /* 将完成的任务追加到 done 链表尾部 (FIFO 入队) */
        if (g_done_tail) g_done_tail->next = j;
        else             g_done_head = j;
        g_done_tail = j;

        pthread_mutex_unlock(&g_lock); // 修改完毕，立即释放锁
    }
    return NULL;
}

/**
 * @brief 初始化 Worker 模块，创建后台工作线程
 */
void worker_init(void)
{
    pthread_create(&g_thread, NULL, worker_main, NULL);
}

/**
 * @brief 向 Worker 投递异步任务 (UI/主线程调用)
 * @param fn   要在后台执行的耗时任务函数
 * @param arg  传递给任务的参数指针 (堆分配内存或静态指针)
 * @param done 任务完成后在主线程执行的回调函数 (如为空则为 Fire-and-Forget 模式)
 */
void worker_post(worker_fn_t fn, void * arg, worker_done_t done)
{
    /* 动态分配一个 job 节点 */
    job_t * j = (job_t *)malloc(sizeof(job_t));
    if (!j) return; /* 内存分配失败防御 */
    j->fn = fn;
    j->arg = arg;
    j->done = done;
    j->next = NULL;

    /* 加锁将任务追加到 pending 队列尾部 */
    pthread_mutex_lock(&g_lock);
    if (g_pending_tail) g_pending_tail->next = j;
    else                g_pending_head = j;
    g_pending_tail = j;

    /* 唤醒处于 pthread_cond_wait 状态的后台 worker 线程 */
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/**
 * @brief 轮询并派发已完成的任务回调 (由主线程/LVGL 定时器高频调用)
 */
void worker_poll(void)
{
    /* 
     * 【精妙设计：原子摘链】
     * 加锁后一次性将整个 done 链表全部拿走（仅消耗几纳秒），
     * 随后立刻释放锁！避免主线程执行回调时长时间持锁阻塞后台线程。
     */
    pthread_mutex_lock(&g_lock);
    job_t * j = g_done_head;
    g_done_head = NULL;
    g_done_tail = NULL;
    pthread_mutex_unlock(&g_lock);

    /* 在无锁保护的环境下逐个处理已完成的任务 */
    while (j) {
        job_t * next = j->next;
        
        if (j->done) {
            /* 1. 如果指定了回调函数：主线程安全执行回调（可在此修改 LVGL 界面） */
            j->done(j->arg);        
        } else {
            /* 2. 如果未指定回调 (done == NULL)：即 Fire-and-Forget 模式，框架自动帮你释放 arg */
            free(j->arg);           
        }

        /* 释放节点自身内存 */
        free(j);
        j = next;
    }
}