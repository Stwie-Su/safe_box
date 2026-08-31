/**
 * @file worker.h
 * 通用异步后台 worker（DESIGN.md §9 多线程设计）。
 *
 * 模型：
 *  - 主线程（LVGL 线程）：所有 LVGL API 只能在这里调用；
 *  - 后台工作线程 ×1：执行阻塞任务（PBKDF2、文件 I/O、网络扫描等）；
 *  - 结果泵：主线程定时调用 worker_poll()，把已完成作业的 done 回调派发回主线程。
 *
 * 约定：
 *  - worker_post 的 arg 必须为堆上分配；提交后主线程不得再读写它；
 *    由 done 回调负责释放（或传 NULL 表示无需回调、框架释放）。
 *  - fn 在【worker 线程】执行，里面绝不能调用任何 LVGL API；
 *  - done 在【主线程】worker_poll() 里执行，可以安全操作 LVGL 控件。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*worker_fn_t)(void * arg);    /* 在后台线程执行 */
typedef void (*worker_done_t)(void * arg);  /* 在主线程 worker_poll() 执行 */

/* 启动后台线程（app_start 早期调用一次） */
void worker_init(void);

/* 提交一个作业：fn(arg) 在后台执行，完成后 done(arg) 在主线程执行。
 * done 可为 NULL（纯 fire-and-forget，框架在后台完成后释放 arg）。 */
void worker_post(worker_fn_t fn, void * arg, worker_done_t done);

/* 主线程轮询：把已完成的作业逐个派发（done 回调）。应在 LVGL 定时器里周期性调用。 */
void worker_poll(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
