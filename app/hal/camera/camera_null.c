/**
 * @file camera_null.c
 * 相机空实现：FM225 的 USB 视频流到货前占位。
 *
 * 所有采集类操作返回 SAFE_ERR_UNSUP，UI 侧据此把预览区显示为占位图案。
 *
 * 【什么时候会走到本后端】
 *   1) 板子/PC 上确实没有摄像头（或 /dev/videoX 打不开、无权限）；
 *   2) 相机比应用枚举得慢，v4l2 首扫失败而降级（见 camera_service.c 的竞态重试）；
 *   3) 显式设了 SAFE_CAMERA_BACKEND=null（PC 无头验证、CTest 里常用）。
 *
 * 【设计要点】「什么都不做」也是一种实现。有了它，上层就不用在每处写
 *   `if (有相机) { ... }`——没有相机时照样调用同一套接口，只是永远拿不到帧。
 *   这种手法叫 **空对象模式（Null Object Pattern）**，比返回 NULL 再到处判空安全。
 */

#include "camera_backend.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------
 * 函数：null_ok（文件内私有）
 * 功能：什么都不做、直接返回成功。
 * 调用者：无（本文件内）——它被填进函数表的 init / deinit / stop 三个槽位，
 *        因此实际调用者是 camera_service.c 里转发 hal_camera_init/deinit/stop 的地方。
 * 数据流：无入参；返回 SAFE_OK。
 * 为什么可以复用同一个函数：这三个动作对「没有相机」而言本就无事可做，
 *   且函数签名一致（void→safe_err_t），故一个实现顶三格。
 * ------------------------------------------------------------------ */
static safe_err_t null_ok(void)
{
    return SAFE_OK;
}

/* ------------------------------------------------------------------
 * 函数：null_start（文件内私有）
 * 功能：拒绝开始采集（本后端没有数据可采）。
 * 调用者：camera_service.c 转发 hal_camera_start() 时。
 * 数据流：入参 (w,h) 被显式 (void) 丢弃（标记「有意不用」，消除 -Wunused 告警）；
 *         返回 SAFE_ERR_UNSUP，上层据此知道「没有相机流」。
 * ------------------------------------------------------------------ */
static safe_err_t null_start(uint16_t w, uint16_t h)
{
    (void)w; (void)h;
    return SAFE_ERR_UNSUP;
}

/* ------------------------------------------------------------------
 * 函数：null_frame（文件内私有）
 * 功能：拒绝取帧。
 * 调用者：camera_service.c 转发 hal_camera_frame() 时。
 * 数据流：两个出参都被 (void) 丢弃且**不写**——调用方必须靠返回值 SAFE_ERR_UNSUP
 *         判断失败，不能去读 rgb565 / info 的内容（它们保持调用前的值）。
 * ------------------------------------------------------------------ */
static safe_err_t null_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    (void)rgb565; (void)info;
    return SAFE_ERR_UNSUP;
}

/* ------------------------------------------------------------------
 * 函数：null_fd（文件内私有）
 * 功能：报告「没有可 poll 的 fd」。
 * 调用者：camera_service.c 转发 hal_camera_fd() 时（face 线程每轮 poll 都会问一次）。
 * 数据流：无入参；恒返回 -1。
 * 为什么是 -1 而不是 0：poll() 会**忽略 negative fd**（revents 恒 0），
 *   这样 face 线程无需为「无相机」写分支，一路复用同一段 poll 代码。
 * ------------------------------------------------------------------ */
static int null_fd(void)
{
    return -1;   /* 无采集 fd：face 线程 poll 时跳过 */
}

/* null 后端实例：注意 release 槽位是 NULL ——函数表中唯一允许为空的字段，
 * 因此 camera_service.c 的 hal_camera_release_frame() 里必须判空后再调用。 */
const hal_camera_backend_t hal_camera_backend_null = {
    "null",
    null_ok,
    null_ok,
    null_start,
    null_ok,
    null_frame,
    NULL,
    null_fd,
};
