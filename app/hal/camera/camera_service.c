/**
 * @file camera_service.c
 * 相机分发层：绑定后端、转发帧请求（FR-16 视频预览）。
 *
 * 后端选择：环境变量 SAFE_CAMERA_BACKEND（v4l2 / null），默认 v4l2；
 * v4l2 初始化失败（无摄像头/无权限）时自动降级 null，不让系统起不来——
 * 与 face_service 的降级策略一致。
 * UI 侧调用 hal_camera_frame() 拿不到帧时，预览区显示占位图案即可。
 *
 * 【本层在链路中的位置】—— 分发层（Facade），**唯一持有后端指针的地方**：
 *
 *    hal_camera_xxx()  ← 业务层只看到这一层
 *           │
 *           ▼
 *    ★ 本文件：选后端 + 转发 + 降级（本文件不含任何 V4L2 代码）
 *           │  s_backend->xxx()
 *           ▼
 *    camera_v4l2.c / camera_null.c（真正干活的实现）
 *
 * 【为什么要这一层】把「用哪个实现」的决策集中到一个文件：
 *   - 业务层永远只写 hal_camera_frame()，换硬件、加后端都不用改业务代码；
 *   - 降级（v4l2 失败 → null）只写一次，不会在每个调用点各写一遍 if。
 */

#include "camera_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const hal_camera_backend_t hal_camera_backend_null;
extern const hal_camera_backend_t hal_camera_backend_v4l2;

static const hal_camera_backend_t * s_backend = &hal_camera_backend_null;
static bool s_started;
static bool s_inited;

/* ------------------------------------------------------------------
 * 函数：find_backend（文件内私有）
 * 功能：按名字在后端注册表里查一个后端；name 为 NULL 时返回表里第一个（表序即优先级）。
 * 调用者：仅 hal_camera_init()。
 * 数据流：入参 name（如 "v4l2"）→ 遍历静态表 → 返回后端 vtable 指针，找不到返回 NULL。
 * 扩展方式：新增后端时，把新后端的 extern 声明加到文件头、实例指针加进 table 即可。
 * ------------------------------------------------------------------ */
static const hal_camera_backend_t * find_backend(const char * name)
{
    const hal_camera_backend_t * table[] = {
        &hal_camera_backend_v4l2,
        &hal_camera_backend_null,
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i] != NULL && (name == NULL || strcmp(name, table[i]->name) == 0))
            return table[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_init
 * 功能：选后端并完成后端初始化；v4l2 起不来时自动降级 null（保证系统可用）。
 * 调用者：app.c 的 app_main()（开机）；hal_camera_start()（未初始化时惰性调用）。
 * 数据流：环境变量 SAFE_CAMERA_BACKEND → find_backend() → 具体后端 init()
 *         → 结果写入文件内静态量 s_backend / s_inited。
 * 注意：s_backend 初值就是 null 后端，所以「还没 init 就调别的接口」也不会空指针。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_init(void)
{
    if (s_inited) return SAFE_OK;

    const char * want = getenv("SAFE_CAMERA_BACKEND");
    const hal_camera_backend_t * b = find_backend((want && *want) ? want : "v4l2");
    if (b == NULL) b = &hal_camera_backend_null;   /* 未知名字 → 降级 */

    safe_err_t e = b->init();
    if (e != SAFE_OK && b != &hal_camera_backend_null) {
        printf("[CAMERA] 后端 %s 初始化失败(%d)，降级 null 预览占位\n", b->name, e);
        b = &hal_camera_backend_null;
        e = b->init();
    }

    s_backend = b;
    s_started = false;
    s_inited  = true;
    printf("[CAMERA] 后端=%s\n", s_backend->name);
    return e;
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_deinit
 * 功能：停流 + 后端收尾（munmap / close），并把 s_backend 复位回 null。
 * 调用者：app.c 的 app_shutdown()（关机）。
 * 数据流：s_backend->stop() → s_backend->deinit() → 复位三个静态状态量。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_deinit(void)
{
    if (!s_inited) return SAFE_OK;
    if (s_started && s_backend->stop) s_backend->stop();
    s_backend->deinit();
    s_backend = &hal_camera_backend_null;
    s_started = false;
    s_inited  = false;
    return SAFE_OK;
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_start
 * 功能：开始采集。额外承担一件事：**开机竞态重试**（相机比应用枚举得慢时补扫）。
 * 调用者：① app.c 开机；② face_service.c 人脸页进入前台（FR-27 duty-cycle 恢复流）。
 * 数据流：(w,h) 期望分辨率 → 后端 start() 协商 → 成功后置 s_started=true。
 * 幂等：已 started 直接返回 SAFE_OK。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_start(uint16_t w, uint16_t h)
{
    if (!s_inited) hal_camera_init();
    if (!s_inited) return SAFE_ERR_STATE;
    if (s_started) return SAFE_OK;

    /* 开机竞态重试（2026-10-02 实测）：应用启动早于 USB 枚举时，v4l2 首扫
     * 找不到相机而降级 null，且 hal_camera_init 是一次性的，此后永远没预览。
     * 人脸页触发 start 时若仍是 null 后端且未显式指定 null，重试 v4l2 初始化
     *（每个进程最多 3 次，覆盖「插上相机才开机/开机慢枚举」两种场景）。 */
    {
        const char * want = getenv("SAFE_CAMERA_BACKEND");
        bool want_null = (want != NULL && strcmp(want, "null") == 0);
        static int s_race_retries = 0;
        if (s_backend == &hal_camera_backend_null && !want_null && s_race_retries < 3) {
            s_race_retries++;
            printf("[CAMERA] 开机竞态重试 %d：重新扫描 v4l2 相机\n", s_race_retries);
            s_inited = false;
            hal_camera_init();
        }
    }

    safe_err_t e = s_backend->start(w, h);
    if (e == SAFE_OK) s_started = true;
    return e;
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_stop
 * 功能：停止采集（转发给后端，保留已申请的缓冲与 fd 以便 start 快速恢复）。
 * 调用者：face_service.c 离开前台；app_shutdown()（幂等）。
 * 数据流：s_backend->stop() → 成功后置 s_started=false。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_stop(void)
{
    if (!s_inited) return SAFE_ERR_STATE;
    if (!s_started) return SAFE_OK;
    safe_err_t e = s_backend->stop();
    if (e == SAFE_OK) s_started = false;
    return e;
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_frame
 * 功能：取最新一帧（纯转发，无额外逻辑）。
 * 调用者：face_thread.c 的 face_capture_once()（唯一调用点，在 face 线程内）。
 * 数据流：(w,h)无关 → 后端 frame() → 出参 rgb565 指向后端内部缓冲、info 填帧元信息。
 * ------------------------------------------------------------------ */
safe_err_t hal_camera_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    if (!s_inited || !s_started) return SAFE_ERR_STATE;
    return s_backend->frame(rgb565, info);
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_release_frame
 * 功能：归还帧所有权（纯转发）。
 * 调用者：face_thread.c 的 face_capture_once()，与 hal_camera_frame() 成对。
 * 数据流：无入参出参；转发到后端 release()（翻转 s_held 标志）。
 * 注意：null 后端的 release 是 NULL，故此处必须判空——这是 vtable 里唯一可空字段。
 * ------------------------------------------------------------------ */
void hal_camera_release_frame(void)
{
    if (s_backend->release) s_backend->release();
}

/* ------------------------------------------------------------------
 * 函数：hal_camera_fd
 * 功能：取底层采集 fd 供调用方 poll()（纯转发）。
 * 调用者：face_thread.c 线程主体（把该 fd 与串口 fd 一起交给 poll）。
 * 数据流：无入参；返回后端 fd()，未初始化时直接返回 -1（poll 忽略负 fd）。
 * ------------------------------------------------------------------ */
int hal_camera_fd(void)
{
    if (!s_inited) return -1;
    return s_backend->fd ? s_backend->fd() : -1;
}
