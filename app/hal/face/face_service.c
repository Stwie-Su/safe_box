/**
 * @file face_service.c
 * 人脸服务门面：绑定后端、转发事件、维护最近一次识别结果。
 *
 * 业务层只跟本文件打交道，不直接碰后端。
 * 换硬件时改的是构建选项，不是这里的代码。
 */

#include "hal/hal_face.h"
#include "face_backend.h"

#include <string.h>

#define MAX_FACE_SUBS   4
#define ENROLL_TIMEOUT_MS  30000

typedef struct {
    face_event_cb_t cb;
    void *          user;
} face_sub_t;

static const face_backend_t * s_backend = NULL;
static face_sub_t             s_subs[MAX_FACE_SUBS];
static face_result_t          s_last;
static bool                   s_has_last;
static bool                   s_running;
static bool                   s_enroll_pending;
static uint32_t               s_enroll_start_ms;
static uint32_t               s_seq;

/* ---------------- 后端注册表 ---------------- */

static const face_backend_t * find_backend(const char * name)
{
    struct { const char * n; const face_backend_t * (*f)(void); } table[] = {
        { "fake",  face_backend_fake  },
        { "fm225", face_backend_fm225 },
        { "none",  face_backend_none  },
    };

    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const face_backend_t * b = table[i].f();
        if(b != NULL && (name == NULL || strcmp(name, b->name) == 0)) return b;
    }
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

safe_err_t face_service_init(const char * backend_name)
{
    if(s_backend != NULL) return SAFE_OK;

    const face_backend_t * b = find_backend(backend_name);
    if(b == NULL) {
        b = face_backend_none();          /* 找不到就退化为空后端，不让系统起不来 */
        if(b == NULL) return SAFE_ERR_FAIL;
    }

    if(b->init) {
        safe_err_t e = b->init();
        if(e != SAFE_OK) return e;
    }

    s_backend = b;
    memset(&s_last, 0, sizeof(s_last));
    s_last.face_id = -1;
    s_has_last     = false;
    s_running      = false;
    s_seq          = 0;
    return SAFE_OK;
}

safe_err_t face_service_deinit(void)
{
    if(s_backend == NULL) return SAFE_OK;
    if(s_running && s_backend->stop) s_backend->stop();
    if(s_backend->deinit) s_backend->deinit();
    s_backend        = NULL;
    s_running        = false;
    s_enroll_pending = false;
    return SAFE_OK;
}

void face_service_tick(uint32_t now_ms)
{
    if(s_backend == NULL) return;
    if(s_backend->tick) s_backend->tick(now_ms);

    /* 录入作业超时保护：后端没在时限内上报结果就报错，避免界面一直转圈 */
    if(s_enroll_pending) {
        if(s_enroll_start_ms == 0) {
            s_enroll_start_ms = now_ms;          /* 受理后的第一个 tick 才开始计时 */
        }
        else if((now_ms - s_enroll_start_ms) > ENROLL_TIMEOUT_MS) {
            s_enroll_pending = false;
            face_enroll_result_t r = { -1, SAFE_ERR_TIMEOUT };
            face_service_emit(FACE_EV_ENROLL_DONE, &r);
        }
    }
}

safe_err_t face_service_subscribe(face_event_cb_t cb, void * user)
{
    if(cb == NULL) return SAFE_ERR_PARAM;
    for(int i = 0; i < MAX_FACE_SUBS; i++) {
        if(s_subs[i].cb == cb && s_subs[i].user == user) return SAFE_OK;
    }
    for(int i = 0; i < MAX_FACE_SUBS; i++) {
        if(s_subs[i].cb == NULL) {
            s_subs[i].cb   = cb;
            s_subs[i].user = user;
            return SAFE_OK;
        }
    }
    return SAFE_ERR_NOMEM;
}

void face_service_unsubscribe(face_event_cb_t cb, void * user)
{
    for(int i = 0; i < MAX_FACE_SUBS; i++) {
        if(s_subs[i].cb == cb && (user == NULL || s_subs[i].user == user)) {
            s_subs[i].cb   = NULL;
            s_subs[i].user = NULL;
        }
    }
}

const face_caps_t * face_service_caps(void)
{
    static face_caps_t caps = { "none", 0, 0 };
    if(s_backend != NULL) {
        caps.name          = s_backend->name;
        caps.caps          = s_backend->caps;
        caps.max_templates = s_backend->max_templates;
    }
    return &caps;
}

safe_err_t face_service_start(void)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(s_running) return SAFE_OK;
    safe_err_t e = s_backend->start ? s_backend->start() : SAFE_OK;
    if(e == SAFE_OK) s_running = true;
    return e;
}

safe_err_t face_service_stop(void)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!s_running) return SAFE_OK;
    safe_err_t e = s_backend->stop ? s_backend->stop() : SAFE_OK;
    if(e == SAFE_OK) s_running = false;
    return e;
}

bool face_service_running(void)
{
    return s_running;
}

safe_err_t face_service_enroll_async(void)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_ENROLL)) return SAFE_ERR_UNSUP;
    if(s_enroll_pending) return SAFE_ERR_BUSY;
    if(s_backend->enroll == NULL) return SAFE_ERR_UNSUP;

    safe_err_t e = s_backend->enroll();
    if(e == SAFE_OK) {
        s_enroll_pending   = true;
        s_enroll_start_ms  = 0;      /* 由 tick 的 now_ms 记账，见 face_service_tick */
    }
    return e;
}

safe_err_t face_service_delete_async(int32_t face_id)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_DELETE)) return SAFE_ERR_UNSUP;
    if(s_backend->delete_tpl == NULL) return SAFE_ERR_UNSUP;
    return s_backend->delete_tpl(face_id);
}

const face_result_t * face_service_last_result(void)
{
    return s_has_last ? &s_last : NULL;
}

safe_err_t face_service_inject(int32_t face_id, int32_t score)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_INJECT)) return SAFE_ERR_UNSUP;
    if(s_backend->inject == NULL) return SAFE_ERR_UNSUP;
    return s_backend->inject(face_id, score);
}

/* 兼容层：旧同步轮询接口。事件驱动改造完成后删除。 */
int face_service_poll_compat(face_result_t * out)
{
    if(out == NULL) return 0;
    if(!s_has_last) return 0;
    *out = s_last;
    s_has_last = false;             /* 取走即清，避免同一结果被消费两次 */
    return 1;
}

/* ---------------- 后端上报入口 ---------------- */

void face_service_emit(face_event_t ev, const void * payload)
{
    if(ev == FACE_EV_DETECT && payload != NULL) {
        s_last     = *(const face_result_t *)payload;
        s_last.seq = ++s_seq;
        s_has_last = true;
        payload    = &s_last;       /* 订阅者拿到的是带序号的副本 */
    }

    if(ev == FACE_EV_ENROLL_DONE || ev == FACE_EV_DELETE_DONE) {
        s_enroll_pending = false;
    }

    for(int i = 0; i < MAX_FACE_SUBS; i++) {
        if(s_subs[i].cb) s_subs[i].cb(ev, payload, s_subs[i].user);
    }
}
