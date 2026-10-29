/**
 * @file worker.h
 * 后台工作线程：把费时的活（算 PIN 哈希、读写文件、扫 WiFi）挪出主线程。
 *
 * 为什么需要：主线程在跑 LVGL，一旦被阻塞，界面就卡住不动。所以约定：
 *   - 主线程（LVGL 线程）：只有它能碰界面控件；
 *   - 后台 worker 线程（1 条）：干活，**绝对不能碰 LVGL**；
 *   - 结果怎么回来：主线程定时调 worker_poll()，在那里执行 done 回调。
 *
 * 三条使用规则：
 *   1) 传给 worker_post 的 arg 必须是堆上分配的，提交后主线程不能再读写它；
 *   2) done 回调里自己 free(arg)；done 传 NULL 表示不需要回调、由框架释放；
 *   3) fn 在后台线程执行，里面不能调任何界面函数。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*worker_fn_t)(void * arg);    /* 后台线程里执行 */
typedef void (*worker_done_t)(void * arg);  /* 主线程 worker_poll() 里执行 */

/* 启动后台线程。开机时调一次（app_main 里）。 */
void worker_init(void);

/* 提交一个作业：fn(arg) 在后台跑，跑完 done(arg) 回到主线程执行。
 * done 可以为 NULL（不需要通知主线程，框架负责释放 arg）。 */
void worker_post(worker_fn_t fn, void * arg, worker_done_t done);

/* 主线程每 20ms 调一次：把已经做完的作业的 done 回调派发出去。 */
void worker_poll(void);

/* 退出：置停止标志、唤醒线程、等它结束（不丢在途作业）。可重复调用。 */
void worker_shutdown(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
