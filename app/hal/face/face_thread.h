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

/* 启动 face 线程。幂等；失败返回错误码。 */
safe_err_t face_thread_start(void);

/* 请求停止：置退出标志 + STREAMOFF 停流 + 静默在途采集（不 join）。
 * 与 hal_camera_deinit / face_thread_join 组合成 §3.2 / b5 的退出顺序：
 * STREAMOFF -> close(fd) -> pthread_join。 */
void face_thread_request_stop(void);

/* 等待 face 线程真正退出（pthread_join）。幂等。 */
void face_thread_join(void);

/* 线程是否在运行。 */
bool face_thread_running(void);

/* 注册 FM225 UART fd（3c 由 fm225 后端提供）。-1 表示未接入，poll 时跳过。
 * 必须在 face_thread_start() 之前调用。 */
void face_thread_set_uart_fd(int fd);

/* 主线程取最新预览帧（RGB565，FACE_THREAD_PREVIEW_W x H）。
 * dst 容量须 >= W*H*2 字节。有比上次更新的帧时拷贝并返回 true，否则 false。
 * 只拷贝、不阻塞采集、不涉及 LVGL。 */
bool face_thread_get_preview(uint16_t * dst, hal_camera_frame_info_t * info);

#ifdef __cplusplus
}
#endif
