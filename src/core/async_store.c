/**
 * @file async_store.c
 * store 异步包装实现：每个包装 = 堆上作业(输入副本 + 输出) + 后台 fn + 主线程 done。
 * 后台 fn 只调 store/backend，绝不碰 LVGL；done 回调里释放作业并把结果交给用户回调。
 */
#include "async_store.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>

/* ---------------- load users ---------------- */
typedef struct {
    astore_users_cb_t cb;
    safe_user_t * list;
    int count;
} lu_job_t;

static void lu_worker(void * p)
{
    lu_job_t * a = (lu_job_t *)p;
    a->list = NULL;
    a->count = 0;
    if (user_load_all(&a->list, &a->count) != 0) a->list = NULL;
}

static void lu_done(void * p)
{
    lu_job_t * a = (lu_job_t *)p;
    if (a->cb) a->cb(a->list, a->count);
    user_list_free(a->list);
    free(a);
}

void astore_load_users(astore_users_cb_t cb)
{
    lu_job_t * a = (lu_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    worker_post(lu_worker, a, lu_done);
}

/* ---------------- query log ---------------- */
typedef struct {
    astore_logs_cb_t cb;
    char evt_filter[24];
    int  has_filter;
    int  res_filter;
    log_entry_t * list;
    int count;
} ql_job_t;

static void ql_worker(void * p)
{
    ql_job_t * a = (ql_job_t *)p;
    a->list = NULL;
    a->count = 0;
    const char * f = a->has_filter ? a->evt_filter : NULL;
    if (log_query(f, a->res_filter, &a->list, &a->count) != 0) a->list = NULL;
}

static void ql_done(void * p)
{
    ql_job_t * a = (ql_job_t *)p;
    if (a->cb) a->cb(a->list, a->count);
    free(a->list);
    free(a);
}

void astore_query_log(const char * evt_filter, int res_filter, astore_logs_cb_t cb)
{
    ql_job_t * a = (ql_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    a->has_filter = (evt_filter != NULL);
    if (evt_filter) {
        strncpy(a->evt_filter, evt_filter, sizeof(a->evt_filter) - 1);
        a->evt_filter[sizeof(a->evt_filter) - 1] = '\0';
    }
    a->res_filter = res_filter;
    worker_post(ql_worker, a, ql_done);
}

/* ---------------- append log (fire-and-forget) ---------------- */
typedef struct {
    char evt[24];
    char user[32];
    char detail[96];
    int  res;
} al_job_t;

static void al_worker(void * p)
{
    al_job_t * a = (al_job_t *)p;
    log_append(a->evt, a->user, a->res, a->detail);
}

void astore_append_log(const char * evt, const char * user, int res, const char * detail)
{
    al_job_t * a = (al_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->evt, evt ? evt : "-", sizeof(a->evt) - 1);
    strncpy(a->user, user ? user : "-", sizeof(a->user) - 1);
    strncpy(a->detail, detail ? detail : "", sizeof(a->detail) - 1);
    a->res = res;
    worker_post(al_worker, a, NULL);   /* done=NULL：后台完成后框架释放 arg */
}

/* ---------------- verify pin (unlock) ---------------- */
typedef struct {
    astore_verify_cb_t cb;
    char pin[16];
    int  result;
    char user[32];
} vp_job_t;

static void vp_worker(void * p)
{
    vp_job_t * a = (vp_job_t *)p;
    a->user[0] = '\0';
    a->result = (int)backend_verify_pin(a->pin, a->user, sizeof(a->user));
}

static void vp_done(void * p)
{
    vp_job_t * a = (vp_job_t *)p;
    if (a->cb) a->cb(a->result, a->user);
    free(a);
}

void astore_verify_pin(const char * pin, astore_verify_cb_t cb)
{
    vp_job_t * a = (vp_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    strncpy(a->pin, pin ? pin : "", sizeof(a->pin) - 1);
    worker_post(vp_worker, a, vp_done);
}

/* ---------------- verify admin pin ---------------- */
typedef struct {
    astore_int_cb_t cb;
    char pin[16];
    int  result;
} va_job_t;

static void va_worker(void * p)
{
    va_job_t * a = (va_job_t *)p;
    a->result = user_verify_pin("admin", a->pin);
}

static void va_done(void * p)
{
    va_job_t * a = (va_job_t *)p;
    if (a->cb) a->cb(a->result);
    free(a);
}

void astore_verify_admin(const char * pin, astore_int_cb_t cb)
{
    va_job_t * a = (va_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    strncpy(a->pin, pin ? pin : "", sizeof(a->pin) - 1);
    worker_post(va_worker, a, va_done);
}

/* ---------------- net get psk ---------------- */
typedef struct {
    astore_str_cb_t cb;
    char ssid[64];
    int  result;
    char psk[64];
} ngp_job_t;

static void ngp_worker(void * p)
{
    ngp_job_t * a = (ngp_job_t *)p;
    a->psk[0] = '\0';
    a->result = net_get_psk(a->ssid, a->psk, sizeof(a->psk));
}

static void ngp_done(void * p)
{
    ngp_job_t * a = (ngp_job_t *)p;
    if (a->cb) a->cb(a->result, a->psk);
    free(a);
}

void astore_net_get_psk(const char * ssid, astore_str_cb_t cb)
{
    ngp_job_t * a = (ngp_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    strncpy(a->ssid, ssid ? ssid : "", sizeof(a->ssid) - 1);
    worker_post(ngp_worker, a, ngp_done);
}

/* ---------------- net add wifi ---------------- */
typedef struct {
    astore_int_cb_t cb;
    char ssid[64];
    char sec[16];
    char psk[64];
    int  result;
} naw_job_t;

static void naw_worker(void * p)
{
    naw_job_t * a = (naw_job_t *)p;
    a->result = net_add_wifi(a->ssid, a->sec, a->psk);
}

static void naw_done(void * p)
{
    naw_job_t * a = (naw_job_t *)p;
    if (a->cb) a->cb(a->result);
    free(a);
}

void astore_net_add_wifi(const char * ssid, const char * sec, const char * psk, astore_int_cb_t cb)
{
    naw_job_t * a = (naw_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    strncpy(a->ssid, ssid ? ssid : "", sizeof(a->ssid) - 1);
    strncpy(a->sec, sec ? sec : "WPA2", sizeof(a->sec) - 1);
    strncpy(a->psk, psk ? psk : "", sizeof(a->psk) - 1);
    worker_post(naw_worker, a, naw_done);
}

/* ---------------- set policy ---------------- */
typedef struct {
    astore_int_cb_t cb;
    int max_failed;
    int lock_seconds;
} sp_job_t;

static void sp_worker(void * p)
{
    sp_job_t * a = (sp_job_t *)p;
    user_policy_set(a->max_failed, a->lock_seconds);
}

static void sp_done(void * p)
{
    sp_job_t * a = (sp_job_t *)p;
    if (a->cb) a->cb(0);
    free(a);
}

void astore_set_policy(int max_failed, int lock_seconds, astore_int_cb_t cb)
{
    sp_job_t * a = (sp_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb = cb;
    a->max_failed = max_failed;
    a->lock_seconds = lock_seconds;
    worker_post(sp_worker, a, sp_done);
}
