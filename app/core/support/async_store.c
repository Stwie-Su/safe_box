/**
 * @file async_store.c
 * store 异步包装实现：每个包装 = 堆上作业(输入副本 + 输出) + 后台 fn + 主线程 done。
 * 后台 fn 只调 store/backend，绝不碰 LVGL；done 回调里释放作业并把结果交给用户回调。
 */
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include <stdlib.h>
#include <string.h>
#include "core/auth/unlock_backend.h"
#include "core/event_bus.h"
#include "core/store/store.h"
#include "hal/hal_time.h"        /* hal_time()：失败锁定截止时刻（AUTH_SIDE_FAIL_BUMP） */

/* ---------------- 加载失败可观测（S1） ----------------
 * 背景：加载**失败**时回调收到的是 (NULL, 0)，与「真的没有数据」完全一样 ——
 * UI 无从区分，会把「读取失败」显示成「暂无用户 / 没有符合条件的记录」，
 * 甚至照常提供「添加用户」入口：用户在文件损坏时点下去，就可能把损坏的
 * 数据文件覆盖掉。
 * 这里只记录「最近一次加载是否失败」，供 UI 查询区分两种情形。
 * 并发：worker 线程写、UI 线程读，均为 bool 标量（单核下标量读写原子）。
 * 注意：**成功时必须清位**，否则一次失败会永久污染后续判断。 */
static volatile bool s_users_load_failed = false;
static volatile bool s_logs_load_failed  = false;

bool astore_users_load_failed(void) { return s_users_load_failed; }
bool astore_logs_load_failed(void)  { return s_logs_load_failed; }

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
    if (user_load_all(&a->list, &a->count) != 0) {
        user_list_free(a->list);         /* QA-09：user_load_all 失败时内部已分配好数组却返回错误，
                                           先释放再置空，避免读盘失败路径内存泄漏 */
        a->list = NULL;
        s_users_load_failed = true;      /* 供 UI 区分「读取失败」与「真的没有」 */
    } else {
        s_users_load_failed = false;     /* 成功必须清位，避免一次失败永久污染 */
    }
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
    if (log_query(f, a->res_filter, &a->list, &a->count) != 0) {
        free(a->list);                   /* QA-09：log_query 失败时内部已分配好数组却返回错误，
                                           先释放再置空，避免读盘失败路径内存泄漏 */
        a->list = NULL;
        s_logs_load_failed = true;
    } else {
        s_logs_load_failed = false;
    }
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

/* ---------------- 用户/凭据变更事件（EV_USER_CHANGED）----------------
 * 为什么不需要后台作业：事件内容在调用方线程里就已经备好（evt/user/detail 都是
 * 定长字符串，不需要读盘），所以这里**不**走 worker_post，直接把 POD 投进总线队列。
 * 用 post 而不是 publish：调用方此刻多半在 worker 线程（变更点即写盘点），
 * 而订阅回调会碰 LVGL —— 必须等主线程 pump 时再派发。
 * 队列满时 event_bus_post 会丢最旧一条：上报是**可丢**的，权威记录是本地 safe.log。 */
void astore_report_user_event(const char * evt, const char * user, int res, const char * detail)
{
    ev_user_changed_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.evt,    evt    ? evt    : "-", sizeof(e.evt)    - 1);
    strncpy(e.user,   user   ? user   : "-", sizeof(e.user)   - 1);
    strncpy(e.detail, detail ? detail : "",  sizeof(e.detail) - 1);
    e.res = res;
    (void)event_bus_post(EV_USER_CHANGED, &e, sizeof(e));
}

/* ---------------- verify admin pin ---------------- */
typedef struct {
    astore_int_cb_t cb;
    char pin[SAFE_VIRTUAL_PIN_MAX_INPUT + 1];
    int  result;
} va_job_t;

static void va_worker(void * p)
{
    va_job_t * a = (va_job_t *)p;
    /* 二级确认要求「任一启用管理员」的 PIN，而非写死名为 "admin" 的用户——
     * 管理员改名 / 多管理员时原来会误判 PIN 错误（用户报：删人脸输入正确 PIN 却报错）。
     * ★ 必须走 backend_verify_admin_pin（精确匹配）：这里曾复用 backend_verify_pin，
     *   而后者会跟随虚位开关做「连续子串」匹配 —— 一旦 FR-18 落地，管理员二次确认
     *   就等于接受了虚位，直接违反规约 §5.5「管理员二次确认：不支持虚位」。 */
    char name[32] = {0};
    unlock_result_t r = backend_verify_admin_pin(a->pin, name, sizeof(name));
    if (r != UNLOCK_OK) { a->result = (r == UNLOCK_LOCKED) ? 2 : 1; return; }
    safe_user_t u;
    if (user_find_by_name(name, &u) == 0 && strcmp(u.role, "admin") == 0)
        a->result = 0;
    else
        a->result = 1;   /* PIN 命中非管理员用户 */
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

/* ---------------- 认证路径的用户表副作用 ----------------
 * 四类「读→改→写」共用一个作业类型 + op 分派。为什么必须整段下沉、
 * 以及顺序保证从哪来，见 async_store.h 的同名段落（那里是权威说明）。 */
typedef struct {
    astore_auth_side_cb_t cb;
    auth_side_op_t op;
    char     user[32];
    char     revoke_detail[96];   /* 删除成功时由本线程落的审计文案；空串 = 不落 */
    int      result;
    int32_t  face_id_rm;
    bool     removed;
} as_job_t;

/* 删除成功后补一条审计（在 worker 内写盘，主线程不碰文件）。
 * 与调用方投的 astore_append_log 作业之间是 FIFO 关系，日志次序与提交次序一致。 */
static void as_log_removed(const as_job_t * a)
{
    if (a->revoke_detail[0] == '\0') return;
    log_append("ALARM", a->user, 1, a->revoke_detail);
}

static void as_worker(void * p)
{
    as_job_t * a = (as_job_t *)p;

    safe_user_t u;
    if (a->user[0] == '\0' || user_find_by_name(a->user, &u) != 0) {
        a->result = -1;   /* 无此用户：副作用无从谈起。不是错误路径，调用方只作提示 */
        return;
    }

    switch (a->op) {
    case AUTH_SIDE_FAIL_BUMP: {
        /* 与 auth_fsm::bump_fail_streak 的原语义一致：达 max_failed 置 lock_until 并清零计数。
         * ★ 读改写由本线程一口气做完 —— 主线程若基于快照算新值会丢失更新（见头文件）。 */
        const safe_policy_t *pol = user_policy();
        u.failed_attempts++;
        if (u.failed_attempts >= pol->max_failed) {
            u.lock_until      = (int64_t)hal_time() + pol->lock_seconds;
            u.failed_attempts = 0;
        }
        a->result = (user_update(&u) == 0) ? 0 : -1;
        break;
    }

    case AUTH_SIDE_UNLOCK_TOUCH: {
        /* FR-9：临时用户每开锁一次计一次，用尽即删（v1.6 起直接删除，不再置停用）。
         * 普通用户（或无次数上限的临时用户）：清零失败计数与锁定。 */
        if (strcmp(u.role, "temp") == 0 && u.use_limit > 0) {
            u.used_count++;
            if (u.used_count >= u.use_limit) {
                int32_t fid = -1;
                if (user_del_cascade(u.id, &fid) == 0) {
                    a->face_id_rm = fid;   /* 仅当模板确实绑在此人身上才回填（FR-21 防误删） */
                    a->removed    = true;
                    a->result     = 0;
                    as_log_removed(a);
                } else {
                    a->result = -1;
                }
                break;
            }
        }
        u.failed_attempts = 0;
        u.lock_until      = 0;
        a->result = (user_update(&u) == 0) ? 0 : -1;
        break;
    }

    case AUTH_SIDE_FACE_INVALIDATE: {
        /* FR-21 防线 1：模组报的名字与本地记录不符 → 本地人脸凭据标记失效。
         * 幂等：已经失效就不写盘，避免无意义的整表重写（save_users 是整表替换）。 */
        if (!u.face_enable) { a->result = 0; break; }
        u.face_enable = false;
        a->result = (user_update(&u) == 0) ? 0 : -1;
        break;
    }

    case AUTH_SIDE_TEMP_REVOKE: {
        int32_t fid = -1;
        if (user_del_cascade(u.id, &fid) == 0) {
            a->face_id_rm = fid;
            a->removed    = true;
            a->result     = 0;
            as_log_removed(a);
        } else {
            a->result = -1;
        }
        break;
    }

    default:
        a->result = -1;
        break;
    }
}

static void as_done(void * p)
{
    as_job_t * a = (as_job_t *)p;
    if (a->cb) a->cb(a->result, a->face_id_rm, a->removed);
    /* ★ 明文敏感数据不残留：本作业只带用户名，仍统一清一次再释放（与 rpc.c 同纪律）。 */
    memset(a->user, 0, sizeof(a->user));
    free(a);
}

void astore_auth_side(auth_side_op_t op, const char *user, const char *revoke_detail,
                      astore_auth_side_cb_t cb)
{
    as_job_t * a = (as_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    a->cb          = cb;
    a->op          = op;
    a->result      = -1;
    a->face_id_rm  = -1;
    a->removed     = false;
    if (user) {
        strncpy(a->user, user, sizeof(a->user) - 1);
        a->user[sizeof(a->user) - 1] = '\0';
    }
    if (revoke_detail) {
        strncpy(a->revoke_detail, revoke_detail, sizeof(a->revoke_detail) - 1);
        a->revoke_detail[sizeof(a->revoke_detail) - 1] = '\0';
    }
    worker_post(as_worker, a, as_done);
}
