/**
 * @file hal_camera.h
 * 相机抽象层对外接口（业务层 / UI 层**只应该包含本头文件**）。
 *
 * 【本层在链路中的位置】—— 自下而上第 3 层，自顶向下看是「业务能看到的全部相机能力」：
 *
 *     业务/UI 层（page_face.c、face_thread.c）
 *              │  调用 hal_camera_xxx()
 *              ▼
 *      ★ 本文件：相机抽象接口（稳定接口约定，不随硬件变）
 *              │  实现在 camera_service.c（分发层，只做转发与降级）
 *              ▼
 *     camera_backend.h：后端 vtable（内部头，业务层禁止包含）
 *              │
 *        ┌─────┴──────────────┐
 *        ▼                    ▼
 *   camera_v4l2.c        camera_null.c
 *   （真摄像头）          （无摄像头占位）
 *
 * 【阅读建议】第一次读本模块，按上面这个顺序自顶向下读；
 *   camera_backend.h 及以下属于实现细节，留到第二步再看。
 *
 * 【注】早期本文件注释写的是「当前为空实现，FM225 未接入」——那句话早已过时，
 *    V4L2 后端（camera_v4l2.c）已落地并长期在用。**过时的注释比没注释更有害**，
 *    本条即为当时那句留下的教训：注释必须随实现同步更新。
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧的元信息：与帧数据一起由 hal_camera_frame() 填好交给调用方。
 * 注意 width/height 是【驱动协商后】的真实分辨率，不一定等于 start() 传入的期望值。 */
typedef struct {
    uint16_t  width;
    uint16_t  height;
    uint32_t  seq;          /* 帧序号（后端内部单调递增，用于判断是否拿到新帧） */
    uint32_t  timestamp;    /* 取帧时刻 hal_time() */
} hal_camera_frame_info_t;

/* ------------------------------------------------------------------
 * 函数：hal_camera_init
 * 功能：初始化相机子系统——选后端、打开设备节点。**不开始出帧**。
 * 调用者：app.c 的 app_main()，开机一次；也可由 hal_camera_start() 内部惰性调用。
 * 数据流：无入参；副作用是内部持有 /dev/videoX 的 fd（fd 生命期 = init..deinit）。
 * 说明：无摄像头 / 打不开时**不返回失败**——内部自动降级 null 后端，保证系统能起来。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_init(void);

/* ------------------------------------------------------------------
 * 函数：hal_camera_deinit
 * 功能：收尾——停流、解映射 mmap、关闭 fd。
 * 调用者：app.c 的 app_shutdown()，且必须排在 face_thread_join() 之后。
 * 数据流：无入参无出参；释放 init 以来持有的全部内核资源。
 * 顺序约束：STREAMOFF(stop) → close(fd)(本函数) → pthread_join，反过来会让线程卡在 DQBUF。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_deinit(void);

/* ------------------------------------------------------------------
 * 函数：hal_camera_start
 * 功能：开始采集——协商分辨率/像素格式、申请并 mmap 驱动缓冲、入队、STREAMON。
 * 调用者：① app.c 开机；② face_service.c 的人脸页「进入前台」（FR-27 duty-cycle 重启流）。
 * 数据流：入参 (w,h) 是【期望】分辨率 → 驱动协商 → 实际值经 hal_camera_frame 的 info 返回。
 * 幂等：已在采集则直接返回 SAFE_OK；可反复 stop/start。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_start(uint16_t w, uint16_t h);

/* ------------------------------------------------------------------
 * 函数：hal_camera_stop
 * 功能：停止采集（STREAMOFF）。已申请的缓冲与 fd **保留**，便于 start 快速恢复。
 * 调用者：face_service.c「离开前台」；app_shutdown()（幂等）。
 * 数据流：无入参无出参；驱动停止向缓冲填帧，fd 变为持续 POLLERR 状态。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_stop(void);

/* ------------------------------------------------------------------
 * 函数：hal_camera_frame
 * 功能：取「最新一帧」的 RGB565 数据（不是队列里最旧的那帧）。
 * 调用者：face_thread.c 的 face_capture_once()——**唯一调用点**，且只在 face 线程内。
 * 数据流：驱动 mmap 缓冲 →(DQBUF)→ 后端内部 s_out 转换缓冲 →(出参 rgb565)→ 调用方。
 *         同时把分辨率/序号/时间戳填进出参 info。
 * ★接口约定：rgb565 指向的是**后端内部缓冲**（非拷贝），调用方用完必须调
 *        hal_camera_release_frame()；漏调则后端停止更新（后续恒返回 BUSY）。
 * 返回值：SAFE_OK 取到帧；SAFE_ERR_BUSY 无新帧（正常，不报错）；其它为硬错误。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info);

/* ------------------------------------------------------------------
 * 函数：hal_camera_release_frame
 * 功能：归还上一帧的所有权（只是翻转一个标志，不释放内存）。
 * 调用者：face_thread.c 的 face_capture_once()，与 hal_camera_frame() 成对出现。
 * 数据流：无入参无出参；清除后端 s_held 标志，允许下一帧被取出。
 * ------------------------------------------------------------------ */
void       hal_camera_release_frame(void);

/* ------------------------------------------------------------------
 * 函数：hal_camera_fd
 * 功能：取出底层采集 fd，供调用方 poll()/select() 复用（IO 复用考点）。
 * 调用者：face_thread.c 的线程主体——把该 fd 与 FM225 串口 fd 一起交给 poll()。
 * 数据流：无入参；返回 /dev/videoX 的 fd，无相机 / 未就绪时返回 -1（poll 会忽略负 fd）。
 * ------------------------------------------------------------------ */
int        hal_camera_fd(void);

#ifdef __cplusplus
}
#endif
