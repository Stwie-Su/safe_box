/**
 * @file face_thread.h
 * face 线程接口（Sprint3 步骤 3b，规约 §3.4 / §3.3 / §5.19）。
 *
 * 一个线程用 poll() 同时收敛两路节拍不同的 IO（IO 复用考点）：
 *   - V4L2 摄像头 video fd（连续流 ~20fps）：取帧 + YUYV->RGB565（只转最新帧）；
 *   - FM225 UART fd（事件驱动，3c 接入）：串口字节 -> 协议状态机。
 *
 * 线程只做采集/转换/解析，**不碰 LVGL**（§3.1：LVGL 仅主线程）；
 * 预览帧写双帧缓冲供主线程取用，识别结果经 event_bus_post(EV_FACE_EVENT) 交主线程。
 *
 * 线程数封顶（§3.3）：主线程 + worker + MQTT + face = 4，本模块是其中第 4 条。
 *
 * 【本线程在整条预览链路中的位置】—— 「采集侧」与「显示侧」的交界：
 *
 *    camera_v4l2.c（取帧 + 转 RGB565，本线程内调用）
 *            │  帧数据
 *            ▼
 *    ★ 本模块：双帧缓冲（生产者）
 *            │  face_thread_get_preview() 拷贝一次
 *            ▼
 *    page_face.c 的 frame_timer_cb()（消费者，LVGL 主线程）
 *            │
 *            ▼
 *    屏幕
 *
 * 【为什么要有这一层缓冲】生产者（相机 ~20fps）与消费者（UI 刷新）节拍不同，
 *   且分属两个线程。双缓冲让两边**互不等待**：采集永远不被 UI 拖慢，
 *   UI 拿不到新帧就用上一帧。这是典型的「单生产者单消费者 + 双缓冲」模型。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"
#include "hal/hal_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 预览帧尺寸（FR-16 锁定 320x240，与 camera 采集一致）。 */
#define FACE_THREAD_PREVIEW_W 320
#define FACE_THREAD_PREVIEW_H 240

/* ------------------------------------------------------------------
 * 函数：face_thread_start
 * 功能：创建并启动 face 线程（幂等：已启动则直接返回 OK）。
 * 调用者：app.c 的 app_main()，且必须排在 face_service_start() 之后
 *        （因为串口 fd 只在 start 前可注册）。
 * 数据流：无入参；内部复位双帧缓冲状态 → pthread_create → 置 s_created。
 * ------------------------------------------------------------------ */
safe_err_t face_thread_start(void);

/* ------------------------------------------------------------------
 * 函数：face_thread_request_stop
 * 功能：「三步退出法」的第一步——置退出标志 + STREAMOFF 停流 + 静默在途采集。
 *        **不 join**（不等线程真的结束），所以调用后线程可能还活着几百微秒。
 * 调用者：app.c 的 app_shutdown()。
 * 数据流：无入参出参；置 s_quit → hal_camera_stop()（唤醒可能阻塞的 poll）
 *         → 抢一次 io 锁再立刻放开（这次加锁会等到「在途采集」那段临界区跑完才返回）。
 * ★ 完整退出顺序（§3.2 / b5，顺序错会卡死）：
 *     request_stop()  →  hal_camera_deinit()  →  face_thread_join()
 *     停流+静默          关 fd（此时已无人在读 mmap）   回收线程
 * ------------------------------------------------------------------ */
void face_thread_request_stop(void);

/* ------------------------------------------------------------------
 * 函数：face_thread_join
 * 功能：等待 face 线程真正退出并回收（pthread_join）。幂等。
 * 调用者：app.c 的 app_shutdown()，排在 hal_camera_deinit() 之后。
 * 数据流：无入参出参；pthread_join 返回后 s_created=false。
 * ------------------------------------------------------------------ */
void face_thread_join(void);

/* ------------------------------------------------------------------
 * 函数：face_thread_running
 * 功能：查询线程主体是否还在跑（退出流程观测用）。
 * 调用者：调试 / 状态打印（page_face.c 的「降级」判断等）。
 * 数据流：无入参；返回 s_running。
 * ------------------------------------------------------------------ */
bool face_thread_running(void);

/* ------------------------------------------------------------------
 * 函数：face_thread_set_uart_fd
 * 功能：把 FM225 串口 fd 借给本线程，让它和相机 fd 一起参与 poll。
 * 调用者：fm225 后端（backend_fm225.c）的 start 流程。
 * 数据流：入参 fd（-1 表示未接入，poll 时自动跳过）；写入 s_uart_fd。
 * ★ 时序约束：必须在 face_thread_start() **之前**调用——线程启动后 s_uart_fd
 *   就成了只读的共享状态，运行期改会与线程读竞争。
 * ------------------------------------------------------------------ */
void face_thread_set_uart_fd(int fd);

/* ------------------------------------------------------------------
 * 函数：face_thread_get_preview  ★消费者侧唯一入口
 * 功能：取最新一帧预览（RGB565），**拷贝到调用方提供的 dst**。
 * 调用者：page_face.c / page_enroll.c 的 frame_timer_cb()（LVGL 主线程，20ms 一次）。
 * 数据流：双帧缓冲 s_fb[s_fb_pub] → memcpy → 出参 dst；同时把帧元信息写进 info。
 * 返回值语义（务必理解）：
 *   true  = 有比上次更新的帧，dst 已被写入新内容；
 *   false = 没有新帧（**dst 内容未定义，不要使用**）——调用方应保持显示上一帧。
 *   这个「有更新才拷贝」的语义避免了无谓的 150KB memcpy。
 * 线程安全：内部持 fb 锁；只拷贝、不阻塞采集、不碰 LVGL。
 * ------------------------------------------------------------------ */
bool face_thread_get_preview(uint16_t * dst, hal_camera_frame_info_t * info);

#ifdef __cplusplus
}
#endif
