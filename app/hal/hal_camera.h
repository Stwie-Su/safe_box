/**
 * @file hal_camera.h
 * 相机的统一接口：界面和人脸线程都只调这里，不直接碰 V4L2。
 *
 * 分了三层，读的时候按这个顺序：
 *   业务 / 界面  →  hal_camera.h（本文件，稳定接口）
 *                →  camera_service.c（选后端、转发、降级）
 *                →  backend_v4l2.c（真正读摄像头） / backend_none.c（没有相机的占位）
 *
 * 打不开摄像头不算失败：内部自动换成空后端，程序照常启动，预览区显示占位。
 *
 * 谁在什么时候调：
 *   - 开机：hal_camera_init() → hal_camera_start()
 *   - face 线程：循环调 hal_camera_frame() 取帧，用完 release
 *   - 退出：先 stop 再 deinit（顺序反了线程会卡在取帧上）
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧的基本信息。width/height 是驱动真正给的分辨率，
 * 不一定等于 start() 时要的那个值。 */
typedef struct {
    uint16_t  width;
    uint16_t  height;
    uint32_t  seq;          /* 帧号，用来判断是不是新的一帧 */
    uint32_t  timestamp;    /* 取帧时刻 */
} hal_camera_frame_info_t;

/* 打开摄像头、选后端，但**不开始出帧**。开机调一次。
 * 没有摄像头或打不开时自动降级成空后端，不返回失败。 */
safe_err_t hal_camera_init(void);

/* 关掉：先停流、再解除内存映射、最后关文件。退出时调，
 * 必须排在 face 线程 join 之前（否则线程可能正卡在读帧上）。 */
safe_err_t hal_camera_deinit(void);

/* 开始采集：和驱动协商分辨率与格式、申请缓冲、开流。
 * (w,h) 是期望值，实际值从 hal_camera_frame 的 info 里拿。
 * 已经在采集就直接返回成功，可以反复 stop/start。 */
safe_err_t hal_camera_start(uint16_t w, uint16_t h);

/* 停流，但保留缓冲和文件描述符，方便下次快速恢复。
 * 人脸页切走、程序退出时调。重复调安全。 */
safe_err_t hal_camera_stop(void);

/* 取最新一帧（RGB565）。
 * 返回 SAFE_OK = 拿到；SAFE_ERR_BUSY = 暂时没新帧（正常，不算错）；其它是硬错误。
 * ★ 拿到的指针指向后端内部缓冲，用完必须调 hal_camera_release_frame()，
 *   否则后端会一直以为帧还被人拿着，后面都返回 BUSY。 */
safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info);

/* 归还上一帧（只是清一个标志，不释放内存）。和 hal_camera_frame 成对调用。 */
void       hal_camera_release_frame(void);

/* 取采集用的文件描述符，给 face 线程 poll 用。没有相机时返回 -1。 */
int        hal_camera_fd(void);

#ifdef __cplusplus
}
#endif
