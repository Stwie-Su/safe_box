/**
 * @file camera_backend.h
 * 相机后端接口（**内部头文件，业务层 / UI 层禁止包含**；请用 hal/hal_camera.h）。
 *
 * 【这是什么】一张函数指针表，两种经典用法：
 *   1) 可替换实现：真摄像头（camera_v4l2.c）与占位（camera_null.c）对上同一套接口，
 *      上层只认接口不认实现 —— 这就是「面向接口编程」在 C 里的写法（结构体 + 函数指针）。
 *   2) 可测试：单测里塞一个假后端，不需要真硬件就能跑通上层逻辑。
 *
 * 【谁用它】只有 camera_service.c 这一处：它持有 `const hal_camera_backend_t * s_backend`，
 *   把 hal_camera_xxx() 的通通转发成 s_backend->xxx()。这就是**分发层（Facade）模式**。
 *
 * 【实例在哪】
 *   - hal_camera_backend_v4l2 定义在 camera_v4l2.c 文件末尾
 *   - hal_camera_backend_null  定义在 camera_null.c  文件末尾
 *   二者都是 `const` 全局对象，编译期就定好，运行期只读不写（也就不涉及并发）。
 */
#pragma once

#include "hal/hal_camera.h"

/* 后端函数表：每个字段的语义与 hal_camera.h 同名函数一一对应，
 * 详细的功能 / 调用者 / 数据流说明见 hal_camera.h，这里不再重复。 */
typedef struct {
    const char * name;                  /* 后端名，用于 SAFE_CAMERA_BACKEND 环境变量匹配 */
    safe_err_t (*init)(void);           /* 打开设备（不开流） */
    safe_err_t (*deinit)(void);         /* 关设备、释放缓冲 */
    safe_err_t (*start)(uint16_t w, uint16_t h);   /* 协商格式 + mmap + STREAMON */
    safe_err_t (*stop)(void);           /* STREAMOFF（保留缓冲，便于快速恢复） */
    safe_err_t (*frame)(const uint8_t ** rgb565, hal_camera_frame_info_t * info);
                                        /* 取最新帧 RGB565（缓冲归后端，需 release） */
    void       (*release)(void);        /* 归还帧所有权 */
    int        (*fd)(void);             /* 采集 fd，供 face 线程 poll；无则 -1 */
} hal_camera_backend_t;
