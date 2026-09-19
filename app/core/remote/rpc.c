/**
 * @file rpc.c
 * RPC 指令分发实现。指令集与错误码严格对应需求文档 2.6 / 2.7。
 *
 * 线程模型：mqtt_start(..., NULL) 不注册回调，消息进入 mqtt_client 内部队列；
 * rpc_poll() 在主线程（LVGL 主循环 20ms 泵）调用 mqtt_take() 取走并处理。
 */
#include "core/remote/rpc.h"
#include "core/remote/rpc_guard.h"  /* req_id 去重 / 时效 / 命令分级（纯逻辑，可单测） */
#include "core/remote/mqtt_client.h"
#include "core/config.h"            /* app_config()：MQTT 连接凭据 */
#include "core/store/store.h"
#include "core/support/worker.h"   /* worker_post：把 store 写操作下沉 worker 线程（QA-20） */
#include "core/auth/totp.h"
#include "hal/hal_time.h"
#include "hal/hal_face.h"
#include "core/auth/unlock_backend.h"
#include "core/auth/auth_fsm.h"
#include "app_version.h"            /* SAFE_VERSION_STRING */
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char   g_local_otp_req[32];
static bool   g_local_otp_pending = false;
static rpc_event_hook_t g_evt_hook = NULL;

/* 近期 req_id 去重表（防重放）。仅在 rpc_poll() 所在的主线程访问
 * （见文件头线程模型：MQTT 线程只入队，主线程出队处理），故无需加锁。 */
static rpc_req_cache_t g_req_cache;

void rpc_set_event_hook(rpc_event_hook_t hook) { g_evt_hook = hook; }

void rpc_init(const char *host, int port)
{
    /* 去重表清零（进程内单实例，一次即可）。 */
    rpc_req_cache_init(&g_req_cache);

    /* 注入 MQTT 连接凭据：来源 SAFE_MQTT_USER / SAFE_MQTT_PASS（缺省为空 →
     * 匿名连接，保持既有 PC 验证路径不变）。必须在 mqtt_start 之前设置。 */
    {
        const app_config_t *cfg = app_config();
        mqtt_set_credentials(cfg ? cfg->mqtt_user : NULL,
                             cfg ? cfg->mqtt_pass : NULL);
    }

    char cid[48];
    snprintf(cid, sizeof(cid), "safe-%d", (int)getpid());
    mqtt_start(host, port, cid, NULL);
}

/* ---- 回执 ---- */
static void ack(const char *req_id, int code, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "code", code);
    cJSON_AddStringToObject(j, "msg", msg ? msg : "");
    if (req_id && *req_id) cJSON_AddStringToObject(j, "req_id", req_id);
    char *s = cJSON_PrintUnformatted(j);
    if (s) { mqtt_publish("safe/log", s, 1, 0); free(s); }
    cJSON_Delete(j);
}

/* ---- 状态快照 ---- */
static char *build_status(int code, const char *msg, const char *req_id)
{
    safe_user_t *us = NULL; int n = 0;
    user_load_all(&us, &n);
    user_list_free(us);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "ts", (double)hal_time());
    cJSON_AddStringToObject(j, "state", auth_fsm_state_name(auth_fsm_state()));
    cJSON_AddNumberToObject(j, "lockout_remain", auth_fsm_lock_remaining());
    cJSON_AddStringToObject(j, "last_reason", face_reason_name(auth_fsm_last_reason()));
    cJSON_AddStringToObject(j, "last_user", auth_fsm_pending_user());
    cJSON_AddNumberToObject(j, "user_count", n);
    cJSON_AddStringToObject(j, "time_src", hal_time_source() == TIME_SRC_RTC ? "rtc" : "sys");
    cJSON_AddNumberToObject(j, "mqtt_online", mqtt_is_connected() ? 1 : 0);
    cJSON_AddStringToObject(j, "fw", SAFE_VERSION_STRING);
    /* FR-23：模组健康纳入远程上报（ok / fail / unknown）。用 cJSON 拼串，不经过定长
     * 缓冲，天然规避 -Wformat-truncation。 */
    cJSON_AddStringToObject(j, "mod_health",
                            face_module_health_name(face_service_module_health()));
    if (code) cJSON_AddNumberToObject(j, "code", code);
    if (msg) cJSON_AddStringToObject(j, "msg", msg);
    if (req_id && *req_id) cJSON_AddStringToObject(j, "req_id", req_id);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

void rpc_publish_status_now(void)
{
    char *s = build_status(0, NULL, NULL);
    if (s) { mqtt_publish("safe/status", s, 0, 0); free(s); }
}

void rpc_publish_event(const char *evt, const char *user, const char *detail, int res)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "ts", (double)hal_time());
    cJSON_AddStringToObject(j, "level", res ? "INFO" : "WARN");
    cJSON_AddStringToObject(j, "event", evt ? evt : "");
    cJSON_AddStringToObject(j, "user", user ? user : "");
    cJSON_AddStringToObject(j, "detail", detail ? detail : "");
    char *s = cJSON_PrintUnformatted(j);
    if (s) { mqtt_publish("safe/log", s, 1, 0); free(s); }
    cJSON_Delete(j);
    if (g_evt_hook) g_evt_hook(evt, user, detail, res);
}

/* 16 字节盐 → 32 字符 hex（与 users.json 的 pin_salt 字段对齐）。
 * store 内部的 to_hex 未导出，这里放一份同语义的本地实现，避免为它开新接口。 */
static void rpc_salt_to_hex(const uint8_t * salt, char out[33])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = HEX[(salt[i] >> 4) & 0x0F];
        out[2 * i + 1] = HEX[salt[i] & 0x0F];
    }
    out[32] = '\0';
}

/* 原因码字符串 → 枚举；非法返回 -1 */
static int parse_face_reason(const char *s)
{
    if (strcmp(s, "ok") == 0)            return (int)FACE_RES_OK;
    if (strcmp(s, "no_match") == 0)      return (int)FACE_RES_NO_MATCH;
    if (strcmp(s, "liveness_fail") == 0) return (int)FACE_RES_LIVENESS_FAIL;
    if (strcmp(s, "timeout") == 0)       return (int)FACE_RES_TIMEOUT;
    if (strcmp(s, "error") == 0)         return (int)FACE_RES_ERROR;
    return -1;
}

/* 校验敏感指令是否「已提供 otp」（字符串非空）。
 * 实际校验（backend_admin_verify_totp，会写盘）下沉到 worker 段执行，
 * 不在主线程做文件 IO——否则与 worker 的落盘并发，破坏
 * 「文件读写由 worker 单线程持有」的不变式（QA-20）。
 * 返回 true 表示 otp 已提供（可进入 worker 校验）；false 表示缺失。 */
static bool rpc_otp_present(cJSON *root, char otp_out[32])
{
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *otp = params ? cJSON_GetObjectItem(params, "otp") : NULL;
    if (cJSON_IsString(otp) && otp->valuestring && *otp->valuestring) {
        strncpy(otp_out, otp->valuestring, 31);
        otp_out[31] = '\0';
        return true;
    }
    return false;
}

/* ================= 异步作业：把 store 写操作挪到 worker 线程 =================
 * 不变式（store.c）：users.json / safe.log 的读写由 worker 单线程持有。
 * 原 dispatch 在主线程直接调 user_add / user_del / user_update /
 * user_policy_set_face / log_append / backend_admin_verify_totp（写盘），
 * 与 worker 的落盘并发 -> 「读-改-写」跨两次 IO 的丢失更新（新建用户消失、
 * 失败计数/锁定被回滚，防暴力锁定失效，QA-20）。
 *
 * 三段划分（不新增线程，复用既有 worker_post）：
 *   ① 解析与准入：cJSON 解析 + 鉴权/时效/去重 + 参数校验，全在主线程且不碰
 *      store 文件；命令参数**按值**拷进 job（绝不带 cJSON 指针，主线程随后
 *      会 cJSON_Delete，指针必悬垂）。
 *   ② 副作用：worker_post 的 fn 在 worker 线程跑——只做 store 写操作及会写盘
 *      的 TOTP 校验，把结果 code/msg 写回 job。
 *   ③ 回执与 UI：done 在主线程跑——发 ack、做 FSM 状态迁移 / 人脸模板删除 /
 *      人脸注入等只能在主线程做的事。 */
typedef enum {
    JOB_ADD_USER,
    JOB_DEL_USER,
    JOB_SET_FACE_ENABLE,
    JOB_SET_FACE_POLICY,
    JOB_REMOTE_UNLOCK,
    JOB_INJECT_FACE_LOG
} rpc_job_kind_t;

typedef struct {
    rpc_job_kind_t kind;
    char  req_id[32];
    int   code;          /* ② 写，③ 读 */
    char  msg[80];       /* ② 写，③ 读 */
    bool  has_otp;
    char  otp[32];       /* 主线程按值拷入，② 在 worker 校验 */

    safe_user_t   u;     /* JOB_ADD_USER / JOB_SET_FACE_ENABLE */
    int     del_uid;
    int32_t del_fid;     /* ② 写（JOB_DEL_USER 回填），③ 读 */
    int     face_otp_after;
    int     face_timeout_s;
    bool    fe_enable;   /* JOB_SET_FACE_ENABLE：启用标志（按值，避免被 user_find 覆盖） */
    bool    unlock_ok;   /* ② 写（JOB_REMOTE_UNLOCK），③ 读 */
    char    inj_detail[96];  /* JOB_INJECT_FACE_LOG */
} rpc_job_t;

/* ② worker 线程：仅做 store 写操作 + 会写盘的 TOTP 校验，结果写回 job。 */
static void rpc_job_fn(void *arg)
{
    rpc_job_t *j = (rpc_job_t *)arg;
    if (j->has_otp) {
        if (backend_admin_verify_totp(j->otp) != AUTH_OK) {
            j->code = RPC_CODE_BAD_OTP;
            snprintf(j->msg, sizeof(j->msg), "动态码错误或已使用");
            return;  /* 校验失败：不再执行后续 store 副作用 */
        }
    }
    switch (j->kind) {
    case JOB_ADD_USER: {
        if (j->u.id <= 0) j->u.id = user_next_id();   /* 分配 id 在 worker（读盘）做 */
        int r = user_add(&j->u);
        if (r == 0) log_append("user_add", "remote", 1, j->u.name);
        j->code = r == 0 ? 0 : 1003;
        snprintf(j->msg, sizeof(j->msg),
                 r == 0 ? "ok" : (r == -2 ? "用户名已存在" : "add failed"));
        break;
    }
    case JOB_DEL_USER: {
        int r = user_del_cascade(j->del_uid, &j->del_fid);
        j->code = r == 0 ? 0 : 2003;
        snprintf(j->msg, sizeof(j->msg), r == 0 ? "ok" : "user not found");
        break;
    }
    case JOB_SET_FACE_ENABLE: {
        if (user_find_by_id(j->u.id, &j->u) != 0) {
            j->code = 2003; snprintf(j->msg, sizeof(j->msg), "user not found");
            break;
        }
        /* 不变式：face_enable == true => face_id >= 0（启用前必须已绑定人脸）。 */
        j->u.face_enable = j->fe_enable;
        if (j->u.face_enable && j->u.face_id < 0) {
            j->code = 1001;
            snprintf(j->msg, sizeof(j->msg), "该用户未绑定人脸，无法启用人脸通道");
            break;
        }
        user_update(&j->u);
        j->code = 0; snprintf(j->msg, sizeof(j->msg), "ok");
        break;
    }
    case JOB_SET_FACE_POLICY: {
        if (j->face_otp_after >= 1 || j->face_timeout_s >= 3) {
            user_policy_set_face(j->face_otp_after, j->face_timeout_s);
            j->code = 0; snprintf(j->msg, sizeof(j->msg), "ok");
        } else {
            j->code = 1001; snprintf(j->msg, sizeof(j->msg), "缺少有效参数");
        }
        break;
    }
    case JOB_REMOTE_UNLOCK: {
        /* TOTP 已在上面校验通过（has_otp 恒 true），此处只记录结果供 ③ 做 FSM。 */
        j->unlock_ok = true;
        j->code = 0; snprintf(j->msg, sizeof(j->msg), "ok");
        break;
    }
    case JOB_INJECT_FACE_LOG: {
        log_append("inject_face", "remote", 1, j->inj_detail);
        break;
    }
    }
}

/* ③ 主线程：发回执 + 只能在主线程做的 UI/FSM/人脸副作用。 */
static void rpc_job_done(void *arg)
{
    rpc_job_t *j = (rpc_job_t *)arg;
    switch (j->kind) {
    case JOB_ADD_USER:
    case JOB_SET_FACE_ENABLE:
    case JOB_SET_FACE_POLICY:
        ack(j->req_id, j->code, j->msg);
        break;
    case JOB_DEL_USER:
        /* 本地删成功且归属核验通过 -> 异步删模组模板（face 线程，主线程调用安全）。 */
        if (j->code == 0 && j->del_fid >= 0) face_service_delete_async(j->del_fid);
        ack(j->req_id, j->code, j->msg);
        break;
    case JOB_REMOTE_UNLOCK:
        /* FR-4 / 验收 8.3：仅在 TOTP 校验通过后触发开锁与状态迁移（UI 弹窗在 main）。 */
        if (j->unlock_ok) auth_fsm_note_unlock("admin");
        ack(j->req_id, j->code, j->msg);
        break;
    case JOB_INJECT_FACE_LOG:
        break;  /* 仅写盘，无主线程副作用 */
    }
    free(j);
}

/* 通道是否被授权执行敏感指令。
 *  - 未配置凭据（SAFE_MQTT_USER/PASS 均空）→ 走旧的「匿名 + OTP」路径（向后兼容，
 *    不改变既有 PC 验证行为）；
 *  - 已配置凭据 → 仅当本端以鉴权身份连上 broker 时才放行敏感指令，匿名连接
 *    只允许只读/非敏感指令。
 * 安全边界：无 TLS 时无法在 payload 层确证「发布者」身份，本判据只能基于本端
 * 连接的鉴权状态做尽最大努力的门控（详见交付报告）。 */
static bool channel_authorized(void)
{
    if (!mqtt_credentials_configured()) return true;
    return mqtt_is_authenticated();
}

/* 调试钩子开关（env SAFE_ALLOW_INJECT）：**白名单式**——仅明确肯定值
 * （"1"/"true"/"yes"/"on"，大小写不敏感）开启；其余（空/"0"/"false"/"no"/"off"/乱写）
 * 一律关闭。默认关死——inject_face 能合成 FACE_RES_OK 走 do_unlock（伪造开锁结果）。
 * 解析下沉 rpc_guard::rpc_env_flag_on（纯逻辑、可单测），避免 "false" 被误当开启。 */
static bool inject_hook_enabled(void)
{
    return rpc_env_flag_on(getenv("SAFE_ALLOW_INJECT"));
}

static void dispatch(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) { ack("", 1002, "JSON 解析失败"); return; }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    cJSON *rid = cJSON_GetObjectItem(root, "req_id");
    char req_id[32] = {0};
    if (cJSON_IsString(rid) && rid->valuestring) {
        strncpy(req_id, rid->valuestring, sizeof(req_id) - 1);
    }
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (!cJSON_IsString(cmd) || !cmd->valuestring) {
        ack(req_id, 1001, "缺少 cmd");
        cJSON_Delete(root);
        return;
    }
    const char *c = cmd->valuestring;

    /* ================= 轻量访问控制（务必在**任何副作用之前**） =================
     * 顺序：鉴权 -> 时效+去重。任一未过立即回执并 return，绝不进入指令分支。 */
    const int64_t now_sec = (int64_t)hal_time();

    if (rpc_cmd_is_sensitive(c) && !channel_authorized()) {
        ack(req_id, RPC_CODE_UNAUTHORIZED,
            "未鉴权通道：敏感指令被拒绝（需配置 MQTT 凭据并以鉴权连接接入）");
        cJSON_Delete(root);
        return;
    }

    if (rpc_cmd_is_debug_hook(c)) {
        int dg = rpc_guard_debug_hook_gate(
            inject_hook_enabled(),
            mqtt_credentials_configured() && mqtt_is_authenticated());
        if (dg != RPC_CODE_OK) {
            ack(req_id, dg,
                (dg == RPC_CODE_DEBUG_DISABLED)
                    ? "调试指令已禁用（需 SAFE_ALLOW_INJECT=1 且鉴权连接）"
                    : "未鉴权通道：调试指令被拒绝");
            cJSON_Delete(root);
            return;
        }
    }

    int64_t req_ts = 0;
    {
        cJSON *tsj = params ? cJSON_GetObjectItem(params, "ts") : NULL;
        if (cJSON_IsNumber(tsj)) req_ts = (int64_t)tsj->valuedouble;
    }
    int gate = rpc_guard_admit(&g_req_cache, c, req_id, req_ts, now_sec, RPC_TS_WINDOW_SEC);
    if (gate == RPC_CODE_MISSING_REQ_ID) {
        ack(req_id, RPC_CODE_MISSING_REQ_ID, "敏感指令缺少 req_id（去重/防重放需要）");
        cJSON_Delete(root);
        return;
    }
    if (gate == RPC_CODE_STALE_TS) {
        ack(req_id, RPC_CODE_STALE_TS, "请求时间戳超出允许窗口（±120s）");
        cJSON_Delete(root);
        return;
    }
    if (gate == RPC_CODE_DUPLICATE) {
        ack(req_id, RPC_CODE_DUPLICATE, "重复请求（req_id 已处理）");
        cJSON_Delete(root);
        return;
    }

    /* ---- query_status：立即回状态（只读，主线程） ---- */
    if (strcmp(c, "query_status") == 0) {
        char *s = build_status(0, NULL, req_id);
        if (s) { mqtt_publish("safe/log", s, 1, 0); free(s); }
        cJSON_Delete(root);
        return;
    }

    /* ---- remote_unlock ---- */
    if (strcmp(c, "remote_unlock") == 0) {
        /* FR-4 / 验收 8.3：锁定期间一律拒绝。 */
        if (auth_fsm_state() == FSM_LOCKOUT) {
            ack(req_id, 3001, "系统处于锁定状态");
            cJSON_Delete(root);
            return;
        }
        cJSON *otp = params ? cJSON_GetObjectItem(params, "otp") : NULL;
        if (cJSON_IsString(otp) && otp->valuestring && *otp->valuestring) {
            /* 带 otp：TOTP 校验（写盘）下沉 worker，FSM 迁移在 ③ 主线程做。 */
            rpc_job_t *j = calloc(1, sizeof(*j));
            if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
            j->kind = JOB_REMOTE_UNLOCK;
            strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
            j->has_otp = true;
            strncpy(j->otp, otp->valuestring, sizeof(j->otp) - 1);
            worker_post(rpc_job_fn, j, rpc_job_done);
        } else {
            /* ADR-4：无 otp -> 触发板子本地弹出动态码页，回 2001（无 store 写，主线程）。 */
            strncpy(g_local_otp_req, req_id, sizeof(g_local_otp_req) - 1);
            g_local_otp_pending = true;
            ack(req_id, 2001, "请在设备输入动态码");
        }
        cJSON_Delete(root);
        return;
    }

    /* ---- add_user（敏感，worker 落盘） ---- */
    if (strcmp(c, "add_user") == 0) {
        char otpbuf[32];
        if (!rpc_otp_present(root, otpbuf)) {
            ack(req_id, 2001, "需要动态码"); cJSON_Delete(root); return;
        }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        cJSON *nm = params ? cJSON_GetObjectItem(params, "name") : NULL;
        cJSON *rl = params ? cJSON_GetObjectItem(params, "role") : NULL;
        cJSON *pn = params ? cJSON_GetObjectItem(params, "pin")  : NULL;
        cJSON *vu = params ? cJSON_GetObjectItem(params, "valid_until") : NULL;
        cJSON *ul = params ? cJSON_GetObjectItem(params, "use_limit")   : NULL;

        if (!(cJSON_IsString(nm) && nm->valuestring && *nm->valuestring)) {
            ack(req_id, 1001, "缺少 name"); cJSON_Delete(root); return;
        }
        if (!(cJSON_IsString(pn) && pn->valuestring && *pn->valuestring)) {
            ack(req_id, 1001, "缺少 pin（PIN 只存哈希，不能为空）"); cJSON_Delete(root); return;
        }
        const char *role = (cJSON_IsString(rl) && rl->valuestring && *rl->valuestring)
                               ? rl->valuestring : "user";
        if (!user_role_valid(role)) {
            ack(req_id, 1001, "role 非法（合法：admin/user/temp）"); cJSON_Delete(root); return;
        }
        const safe_policy_t *pol = user_policy();
        const size_t plen = strlen(pn->valuestring);
        if (plen < (size_t)pol->pin_min_len || plen > (size_t)pol->pin_max_len) {
            ack(req_id, 1001, "pin 长度不在策略允许范围内"); cJSON_Delete(root); return;
        }
        int64_t valid_until = 0;
        int     use_limit   = 0;
        if (cJSON_IsNumber(vu)) valid_until = (int64_t)vu->valuedouble;
        if (cJSON_IsNumber(ul)) use_limit   = (int)ul->valuedouble;
        if (valid_until < 0 || use_limit < 0) {
            ack(req_id, 1001, "valid_until / use_limit 不能为负"); cJSON_Delete(root); return;
        }
        if (valid_until > 0 && valid_until <= (int64_t)hal_time()) {
            ack(req_id, 1001, "valid_until 必须晚于当前时间"); cJSON_Delete(root); return;
        }

        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_ADD_USER;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        j->has_otp = true;
        strncpy(j->otp, otpbuf, sizeof(j->otp) - 1);
        /* 主线程：按值构建（PBKDF2 在主线程算，纯 CPU 不算磁盘 IO）。id 在 ② 分配。 */
        memset(&j->u, 0, sizeof(j->u));
        j->u.id = uid;
        strncpy(j->u.name, nm->valuestring, sizeof(j->u.name) - 1);
        strncpy(j->u.role, role, sizeof(j->u.role) - 1);
        strncpy(j->u.auth_method, "pin", sizeof(j->u.auth_method) - 1);
        j->u.enabled = true;
        j->u.face_id = -1;
        j->u.valid_until = valid_until;
        j->u.use_limit   = use_limit;
        {
            time_t tn = (time_t)hal_time();
            struct tm tmv;
            localtime_r(&tn, &tmv);
            strftime(j->u.created_at, sizeof(j->u.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);
        }
        uint8_t salt[16];
        if (pin_hash(pn->valuestring, salt, j->u.pin_hash) != 0) {
            ack(req_id, 1003, "pin 哈希失败"); free(j); cJSON_Delete(root); return;
        }
        rpc_salt_to_hex(salt, j->u.pin_salt);
        worker_post(rpc_job_fn, j, rpc_job_done);
        cJSON_Delete(root);
        return;
    }

    /* ---- del_user（敏感，worker 落盘） ---- */
    if (strcmp(c, "del_user") == 0) {
        char otpbuf[32];
        if (!rpc_otp_present(root, otpbuf)) {
            ack(req_id, 2001, "需要动态码"); cJSON_Delete(root); return;
        }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_DEL_USER;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        j->has_otp = true;
        strncpy(j->otp, otpbuf, sizeof(j->otp) - 1);
        j->del_uid = uid;
        worker_post(rpc_job_fn, j, rpc_job_done);
        cJSON_Delete(root);
        return;
    }

    /* ---- set_face_enable（敏感，worker 落盘） ---- */
    if (strcmp(c, "set_face_enable") == 0) {
        char otpbuf[32];
        if (!rpc_otp_present(root, otpbuf)) {
            ack(req_id, 2001, "需要动态码"); cJSON_Delete(root); return;
        }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        int en = params && cJSON_GetObjectItem(params, "enable") ? cJSON_GetObjectItem(params, "enable")->valueint : 0;
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_SET_FACE_ENABLE;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        j->has_otp = true;
        strncpy(j->otp, otpbuf, sizeof(j->otp) - 1);
        /* 读取+校验（face_id<0 拒绝启用）下沉 ② worker；此处只传初值。 */
        memset(&j->u, 0, sizeof(j->u));
        j->u.id = uid;
        j->fe_enable = en ? true : false;
        worker_post(rpc_job_fn, j, rpc_job_done);
        cJSON_Delete(root);
        return;
    }

    /* ---- set_face_policy（敏感，worker 落盘） ---- */
    if (strcmp(c, "set_face_policy") == 0) {
        char otpbuf[32];
        if (!rpc_otp_present(root, otpbuf)) {
            ack(req_id, 2001, "需要动态码"); cJSON_Delete(root); return;
        }
        int otp_after = params && cJSON_GetObjectItem(params, "face_otp_after")
                        ? cJSON_GetObjectItem(params, "face_otp_after")->valueint : 0;
        int timeout   = params && cJSON_GetObjectItem(params, "face_verify_timeout_s")
                        ? cJSON_GetObjectItem(params, "face_verify_timeout_s")->valueint : 0;
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_SET_FACE_POLICY;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        j->has_otp = true;
        strncpy(j->otp, otpbuf, sizeof(j->otp) - 1);
        j->face_otp_after = otp_after;
        j->face_timeout_s = timeout;
        worker_post(rpc_job_fn, j, rpc_job_done);
        cJSON_Delete(root);
        return;
    }

    /* ---- sync_time（敏感，已收编：强制鉴权 + req_id 去重；ts 时效豁免保留） ---- */
    if (strcmp(c, "sync_time") == 0) {
        int ts = params && cJSON_GetObjectItem(params, "ts") ? cJSON_GetObjectItem(params, "ts")->valueint : 0;
        if (ts > 0) { hal_time_set((uint32_t)ts); ack(req_id, 0, "ok"); }
        else ack(req_id, 1001, "invalid ts");
        cJSON_Delete(root);
        return;
    }

    /* ---- submit_otp（调试用，受 WAIT_OTP 门控；FSM 状态迁移必须在主线程） ---- */
    if (strcmp(c, "submit_otp") == 0) {
        cJSON *otp = params ? cJSON_GetObjectItem(params, "otp") : NULL;
        if (!(cJSON_IsString(otp) && otp->valuestring && *otp->valuestring)) {
            ack(req_id, 2001, "需要动态码");
            cJSON_Delete(root);
            return;
        }
        auth_fsm_submit_otp(otp->valuestring);
        ack(req_id, 0, "submitted");
        cJSON_Delete(root);
        return;
    }

    /* ---- inject_face（调试钩子，取代旧 inject_score）---- */
    if (strcmp(c, "inject_face") == 0) {
        cJSON *rs = params ? cJSON_GetObjectItem(params, "reason") : NULL;
        int fid = params && cJSON_GetObjectItem(params, "face_id") ? cJSON_GetObjectItem(params, "face_id")->valueint : -1;
        int reason_i = parse_face_reason(cJSON_IsString(rs) && rs->valuestring ? rs->valuestring : "");
        if (reason_i < 0) {
            ack(req_id, 1001, "invalid reason（合法：ok/no_match/liveness_fail/timeout/error）");
            cJSON_Delete(root);
            return;
        }
        /* 人脸注入在板子管线（主线程，无磁盘 IO）；审计日志写盘下沉 worker。 */
        face_service_inject(fid, (face_reason_t)reason_i);
        char detail[96];
        snprintf(detail, sizeof(detail), "face_id=%d reason=%.32s",
                 fid, cJSON_IsString(rs) && rs->valuestring ? rs->valuestring : "?");
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (j) {
            j->kind = JOB_INJECT_FACE_LOG;
            strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
            strncpy(j->inj_detail, detail, sizeof(j->inj_detail) - 1);
            worker_post(rpc_job_fn, j, rpc_job_done);
        } else {
            log_append("inject_face", "remote", 1, detail);  /* 极端：分配失败退回主线程写 */
        }
        ack(req_id, 0, "injected");
        cJSON_Delete(root);
        return;
    }

    ack(req_id, 1003, "未知指令");
    cJSON_Delete(root);
}

void rpc_poll(void)
{
    char topic[128], payload[512];
    int processed = 0;
    const uint32_t budget_start = hal_time_ms();
    while (mqtt_take(topic, sizeof(topic), payload, sizeof(payload))) {
        dispatch(payload);
        if (++processed >= RPC_POLL_MAX_MSGS) break;
        if (hal_time_ms() - budget_start >= RPC_POLL_MAX_MS) break;
    }
}

bool rpc_take_local_otp(char *req_id, size_t cap)
{
    if (!g_local_otp_pending) return false;
    strncpy(req_id, g_local_otp_req, cap - 1);
    req_id[cap - 1] = '\0';
    g_local_otp_pending = false;
    g_local_otp_req[0] = '\0';
    return true;
}

