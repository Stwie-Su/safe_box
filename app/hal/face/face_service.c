/**
 * @file face_service.c
 * 人脸服务门面：绑定后端、转发事件、维护最近一次识别结果。
 *
 * 业务层只跟本文件打交道，不直接碰后端。
 * 换硬件时改的是构建选项，不是这里的代码。
 *
 * 事件通道（步骤 3a）：自有订阅数组已拆除，结果统一经
 * event_bus(EV_FACE_EVENT) 广播（POD payload ev_face_event_t，§5.18 两套机制收敛）。
 * emit 统一走 post 入队（3b 起）；调用方可以是主线程（fake 后端 tick）也可以是
 * face 线程（fm225 后端在 face 线程解析识别结果，步骤 3c）——s_last/s_seq 的
 * 读写由 s_last_lock 保护（§5.11 并发条款），锁外 post 无跨线程悬空。
 */

#include "hal/hal_face.h"
#include "face_backend.h"
#include "hal/hal_camera.h"
/* ⚠️ 已知分层例外（**不是笔误，勿按 backend_fm225.c 那条「hal->core 会 undefined
 * reference」的注释来“修”这里**）：hal/face 需要把识别结果上报给 core 的事件总线，
 * 故本文件确实 include core/event_bus.h 并调用 event_bus_post —— 这是 hal->core 的
 * 反向依赖，已登记为架构债（技术路线规约 §10）。根治方案：把结果上报改为由 core 向
 * hal 注入回调（依赖倒置），届时本 include 即可移除。
 * 注：tools/check_layers.sh 只校验 core（不碰 LVGL/平台头）与 hal 公共头洁净，未覆盖
 * hal->core 方向 —— 属门禁盲区（QA 发现 C3，待补检查）。 */
#include "core/event_bus.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ENROLL_TIMEOUT_MS  30000

static const face_backend_t * s_backend = NULL;
static face_result_t          s_last;
static bool                   s_has_last;
static bool                   s_running;
static bool                   s_enroll_pending;
static uint32_t               s_enroll_start_ms;
static uint32_t               s_seq;

/* 最近结果锁（步骤 3c，规约 §5.11 并发条款）：emit 可能来自 face 线程（fm225 后端
 * 在 face 线程解析识别结果），而 last_result / poll_compat 在主线程被 UI/RPC 调用——
 * s_last / s_seq / s_has_last 的读写须持本锁。锁只覆盖字段读写，不含 event_bus_post
 * （§3.1 不持锁调回调；post 自身有队列锁，无需外层保护）。 */
static pthread_mutex_t s_last_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* ---------------- Duty-cycle（FR-27，规约 §5.20） ---------------- */

/* 前台标志：主线程写（page_face 可见性边沿），face 线程 / 后端 tick 读。
 * 跨线程仅此一个 volatile 标量，与 face_thread 的退出标志同一纪律（§3.4）。 */
static volatile bool s_fg = true;

void face_service_set_foreground(bool active)
{
    if(s_fg == active) return;
    s_fg = active;
    /* 相机流跟着前台走：STREAMOFF 后模组 UVC 传感断电（stop 幂等）；恢复时按
     * 预览契约的 320x240 重启。识别会话由 fm225 后端在 tick 里响应该标志
     * （0x10 RESET 终止 / REARM 退避自然重开）。 */
    if(active) {
        safe_err_t e = hal_camera_start(320, 240);
        if(e != SAFE_OK) printf("[face] 相机恢复失败（err=%d）\n", (int)e);
    }
    else {
        hal_camera_stop();
    }
    printf("[face] %s：相机%s，识别会话%s\n",
           active ? "进入前台" : "离开前台",
           active ? "恢复" : "停流",
           active ? "恢复" : "终止");
}

bool face_service_foreground(void)
{
    return s_fg;
}

safe_err_t face_service_enroll_async(const char * user_name)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_ENROLL)) return SAFE_ERR_UNSUP;
    if(s_enroll_pending) return SAFE_ERR_BUSY;
    if(s_backend->enroll == NULL) return SAFE_ERR_UNSUP;

    /* user_name 随录入写入模组侧模板（FR-21 防线 1）；不支持写名的后端忽略该参数即可。 */
    safe_err_t e = s_backend->enroll(user_name);
    if(e == SAFE_OK) {
        s_enroll_pending   = true;
        s_enroll_start_ms  = 0;      /* 由 tick 的 now_ms 记账，见 face_service_tick */
    }
    return e;
}

int32_t face_service_module_users(int32_t * ids, int32_t cap)
{
    /* 后端未编入实现 → 视为「无此概念」（-1）；有实现但尚未取得 → 由后端返回 -2 */
    if(s_backend == NULL || s_backend->module_users == NULL) return -1;
    return s_backend->module_users(ids, cap);
}

face_module_health_t face_service_module_health(void)
{
    /* 后端未编入健康监测 → 未知（fake / none 无模组级健康概念） */
    if(s_backend == NULL || s_backend->module_health == NULL) return FACE_MOD_UNKNOWN;
    return s_backend->module_health();
}

safe_err_t face_service_delete_async(int32_t face_id)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_DELETE)) return SAFE_ERR_UNSUP;
    if(s_backend->delete_tpl == NULL) return SAFE_ERR_UNSUP;
    return s_backend->delete_tpl(face_id);
}

safe_err_t face_service_verify_once(void)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_DETECT)) return SAFE_ERR_UNSUP;
    if(s_backend->verify_once == NULL) return SAFE_ERR_UNSUP;
    return s_backend->verify_once();
}

const face_result_t * face_service_last_result(void)
{
    pthread_mutex_lock(&s_last_lock);
    static face_result_t snap;             /* 快照：返回指针归本模块所有，解锁后仍有效 */
    bool has = s_has_last;
    if(has) snap = s_last;
    pthread_mutex_unlock(&s_last_lock);
    return has ? &snap : NULL;
}

safe_err_t face_service_inject(int32_t face_id, face_reason_t reason)
{
    if(s_backend == NULL) return SAFE_ERR_STATE;
    if(!(s_backend->caps & FACE_CAP_INJECT)) return SAFE_ERR_UNSUP;
    if(s_backend->inject == NULL) return SAFE_ERR_UNSUP;
    return s_backend->inject(face_id, reason);
}

/* 兼容层：旧同步轮询接口。事件驱动改造完成后删除。 */
int face_service_poll_compat(face_result_t * out)
{
    if(out == NULL) return 0;
    pthread_mutex_lock(&s_last_lock);
    int has = s_has_last ? 1 : 0;
    if(has) *out = s_last;
    s_has_last = false;             /* 取走即清，避免同一结果被消费两次 */
    pthread_mutex_unlock(&s_last_lock);
    return has;
}

/* ---------------- 后端上报入口 ---------------- */

const char * face_reason_name(face_reason_t r)
{
    switch(r) {
        case FACE_RES_OK:           return "ok";
        case FACE_RES_NO_MATCH:     return "no_match";
        case FACE_RES_LIVENESS_FAIL:return "liveness_fail";
        case FACE_RES_TIMEOUT:      return "timeout";
        case FACE_RES_ERROR:        return "error";
        default:                    return "unknown";
    }
}

/* 健康三态名（FR-23，RPC status / 日志用） */
const char * face_module_health_name(face_module_health_t h)
{
    switch(h) {
        case FACE_MOD_OK:   return "ok";
        case FACE_MOD_FAIL: return "fail";
        default:            return "unknown";
    }
}

/* emit：后端结果的唯一出口，组装 ev_face_event_t 后经总线广播。
 * FACE_EV_ERROR 的 payload 为 const char *（错误描述），拷进 msg[64] 截断。 */
void face_service_emit(face_event_t ev, const void * payload)
{
    ev_face_event_t e;
    memset(&e, 0, sizeof(e));
    e.ev = ev;

    if(ev == FACE_EV_DETECT && payload != NULL) {
        /* 最近结果读写持锁（§5.11）：emit 可能来自 face 线程，与主线程
         * last_result / poll_compat 并发。e.res 取锁内快照，post 放锁外（§3.1）。 */
        pthread_mutex_lock(&s_last_lock);
        s_last     = *(const face_result_t *)payload;
        s_last.seq = ++s_seq;
        s_has_last = true;
        e.res      = s_last;      /* 订阅者拿到的是带序号的副本 */
        pthread_mutex_unlock(&s_last_lock);
    }
    else if(ev == FACE_EV_ENROLL_DONE && payload != NULL) {
        e.enroll = *(const face_enroll_result_t *)payload;
    }
    else if(ev == FACE_EV_DELETE_DONE && payload != NULL) {
        e.del = *(const face_delete_result_t *)payload;
    }
    else if(ev == FACE_EV_ERROR && payload != NULL) {
        const char * msg = (const char *)payload;
        strncpy(e.msg, msg, sizeof(e.msg) - 1);   /* 定长截断，msg 已零初始化保 NUL 结尾 */
    }
    else {
        return;   /* 未知事件类型或缺少 payload：不上总线 */
    }

    if(ev == FACE_EV_ENROLL_DONE || ev == FACE_EV_DELETE_DONE) {
        s_enroll_pending = false;
    }

    /* 统一走 post 入队：脸结果可能来自 face 线程（3c），由主线程 event_bus_pump()
     * 派发（规约 §3.4）；主线程调用亦安全，仅延迟一拍。payload 为 POD 按值拷贝。 */
    event_bus_post(EV_FACE_EVENT, &e, sizeof(e));
}
