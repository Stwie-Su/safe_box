/**
 * @file backend_fake.c
 * 模拟人脸后端：不碰硬件，靠注入产生识别结果。
 *
 * 用途：FM225 到货前，在 PC 上完整验证「注入分数 → 分级 → 动态码 → 开锁 → 上报」链路。
 * 录入与识别都做延时上报，行为与真实模组一致（异步、有耗时），
 * 这样换成真实后端时业务层的时序不会变。
 */

#include "face_backend.h"

#include "hal/hal_time.h"

#include <string.h>

#define ENROLL_COST_MS   1200    /* 模拟录入耗时 */
#define DETECT_LATENCY_MS  20    /* 模拟识别出结果耗时 */

static bool     s_started;
static bool     s_enroll_job;
static uint32_t s_enroll_done_ms;
static int32_t  s_next_face_id = 100;

static bool     s_detect_job;
static uint32_t s_detect_at_ms;
static int32_t  s_detect_face_id;
static face_reason_t s_detect_reason;

static safe_err_t fake_init(void)
{
    s_started        = false;
    s_enroll_job     = false;
    s_detect_job     = false;
    s_next_face_id   = 100;
    return SAFE_OK;
}

static safe_err_t fake_deinit(void)
{
    s_started = false;
    return SAFE_OK;
}

static safe_err_t fake_start(void)
{
    s_started = true;
    return SAFE_OK;
}

static safe_err_t fake_stop(void)
{
    s_started    = false;
    s_detect_job = false;
    return SAFE_OK;
}

/* 延时基准用 0 表示"尚未定基准"，由作业受理后的第一个 tick 赋值 */
static void fake_tick(uint32_t now_ms)
{
    if(s_enroll_job) {
        if(s_enroll_done_ms == 0) s_enroll_done_ms = now_ms + ENROLL_COST_MS;
        else if(now_ms >= s_enroll_done_ms) {
            s_enroll_job = false;
            face_enroll_result_t r = { s_next_face_id++, SAFE_OK };
            face_service_emit(FACE_EV_ENROLL_DONE, &r);
        }
    }

    if(s_started && s_detect_job) {
        if(s_detect_at_ms == 0) s_detect_at_ms = now_ms + DETECT_LATENCY_MS;
        else if(now_ms >= s_detect_at_ms) {
            s_detect_job = false;
            face_result_t r;
            memset(&r, 0, sizeof(r));
            r.face_id   = s_detect_face_id;
            r.reason    = s_detect_reason;
            /* 契约（hal_face.h）：timestamp = hal_time()（Unix 秒），不是"进程/开机起算秒"。
             * 消费方 page_face 用 hal_time() 判新鲜度，用 tick 时钟会让结果恒判过期（D12）。 */
            r.timestamp = hal_time();
            face_service_emit(FACE_EV_DETECT, &r);
        }
    }
}

static safe_err_t fake_enroll(void)
{
    if(s_enroll_job) return SAFE_ERR_BUSY;
    s_enroll_job     = true;
    s_enroll_done_ms = 0;
    return SAFE_OK;
}

static safe_err_t fake_delete_tpl(int32_t face_id)
{
    if(face_id < 0) return SAFE_ERR_PARAM;
    face_delete_result_t r = { face_id, SAFE_OK };
    face_service_emit(FACE_EV_DELETE_DONE, &r);
    return SAFE_OK;
}

static safe_err_t fake_inject(int32_t face_id, face_reason_t reason)
{
    if(reason < FACE_RES_OK || reason > FACE_RES_ERROR) return SAFE_ERR_PARAM;
    s_detect_face_id = face_id;
    s_detect_reason  = reason;
    s_detect_job     = true;
    s_detect_at_ms   = 0;
    return SAFE_OK;
}

static const face_backend_t backend = {
    .name          = "fake",
    .caps          = FACE_CAP_DETECT | FACE_CAP_ENROLL | FACE_CAP_DELETE | FACE_CAP_INJECT,
    .max_templates = -1,
    .init          = fake_init,
    .deinit        = fake_deinit,
    .start         = fake_start,
    .stop          = fake_stop,
    .tick          = fake_tick,
    .enroll        = fake_enroll,
    .delete_tpl    = fake_delete_tpl,
    .inject        = fake_inject,
};

const face_backend_t * face_backend_fake(void)
{
    return &backend;
}

