/**
 * @file face_thread.h
 * 人脸线程：一个线程同时等两路硬件，用 poll() 一起管。
 *
 *   ① 摄像头（每秒约 20 帧）：取一帧、转成 RGB565，放进双缓冲；
 *   ② FM225 串口（有数据才来）：把字节喂给协议解析器，解析出结果就发事件。
 *
 * 为什么用一条线程管两路：两路速度差很多，开两条线程没必要；
 * poll() 可以同时等两个 fd，谁有数据就处理谁（这也是面试常问的 IO 复用）。
 *
 * 这条线程**绝对不能碰界面**（LVGL 只能在主线程用）。它只做两件事：
 *   - 把预览帧写进双缓冲，主线程自己来取；
 *   - 把识别结果通过事件总线发出去，主线程收到后更新界面。
 *
 * 双缓冲的作用：采集和显示速度不一样，用两块缓冲各写各的，
 * 采集不用等界面画完，界面拿不到新帧就继续显示上一帧。
 *
 * 启动 / 退出顺序：
 *   启动：hal_face_start()（把串口 fd 交给本线程）→ face_thread_start()
 *   退出：face_thread_request_stop() → hal_camera_deinit() → face_thread_join()
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"
#include "hal/hal_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 预览帧尺寸，和相机采集一致。 */
#define FACE_THREAD_PREVIEW_W 320
#define FACE_THREAD_PREVIEW_H 240

/* 创建并启动线程（已经启动就直接返回成功）。必须在 hal_face_start() 之后调，
 * 因为串口 fd 只有在线程启动前才能交给它。 */
safe_err_t face_thread_start(void);

/* 第一步退出：置退出标志、停掉采集流、等在途的那次取帧结束。**不等线程结束**。
 * 调用后线程可能还活几百微秒，所以后面还要 join。 */
void face_thread_request_stop(void);

/* 第二步退出：等线程真正结束并回收。在 hal_camera_deinit() 之后调。 */
void face_thread_join(void);

/* 线程还在跑吗（排查问题时看）。 */
bool face_thread_running(void);

/* 把 FM225 串口的 fd 交给线程，让它和相机一起 poll。
 * fd 传 -1 表示没有串口。★ 必须在线程启动**之前**调。 */
void face_thread_set_uart_fd(int fd);

/* 取最新一帧预览，拷贝到 dst。
 * 返回 true = 有更新的帧，dst 已是新内容；
 * 返回 false = 没有新帧，**dst 里是什么不要用**，继续显示上一帧就行。
 * 在主线程的界面定时器里调（20ms 一次）。 */
bool face_thread_get_preview(uint16_t * dst, hal_camera_frame_info_t * info);

#ifdef __cplusplus
}
#endif
