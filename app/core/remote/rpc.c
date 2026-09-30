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
#include "core/store/store_cache.h"  /* 主线程只读快照（QA-20 不变式：读写全归 worker） */
#include "core/support/worker.h"   /* worker_post：把 store 写操作下沉 worker 线程（QA-20） */
#include "core/support/async_store.h"  /* astore_report_user_event：变更事件上总线（P6） */
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

        /* R9 T06：MQTT over TLS（默认关闭 = 明文，保持既有 PC 验证路径）。
         * 必须在 mqtt_start 之前设置。 */
        if (cfg != NULL && cfg->mqtt_tls) {
            mqtt_set_tls(true,
                         cfg->mqtt_ca_file,
                         cfg->mqtt_cert_file[0] ? cfg->mqtt_cert_file : NULL,
                         cfg->mqtt_key_file[0]  ? cfg->mqtt_key_file  : NULL,
                         cfg->mqtt_tls_verify);
        }
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

/* ---- 状态快照 ----
 * ★ QA-21：这里**不能**调整表读盘接口 —— 整份 users.json 读出来再 cJSON 全表
 *   解析，在 A7 上要几百毫秒，会把 20ms 的 LVGL 泵卡死一拍（`RPC_POLL_MAX_MS 8`
 *   这种「两条指令之间」的预算防不住它，因为它是**单条指令内部**的耗时）。
 *   改读 store_cache 快照：worker 每次落盘后刷新，主线程只读、零阻塞。
 *   user_count 由调用方传入（来自快照），本函数不碰文件。 */
static char *build_status(int code, const char *msg, const char *req_id, int user_count)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "ts", (double)hal_time());
    cJSON_AddStringToObject(j, "state", auth_fsm_state_name(auth_fsm_state()));
    cJSON_AddNumberToObject(j, "lockout_remain", auth_fsm_lock_remaining());
    cJSON_AddStringToObject(j, "last_reason", face_reason_name(auth_fsm_last_reason()));
    cJSON_AddStringToObject(j, "last_user", auth_fsm_pending_user());
    cJSON_AddNumberToObject(j, "user_count", user_count);
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
    /* 快照读：不落盘、不阻塞。stamp==0（worker 还没发布过）时 user_count 为 0，
     * 语义是「未知」而非「确实没有用户」—— 启动后第一次落盘即会填上真值。 */
    int user_count = 0;
    store_cache_user_count(&user_count, NULL);
    char *s = build_status(0, NULL, NULL, user_count);
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
    JOB_INJECT_FACE_LOG,
    JOB_SET_TIME,           /* QA-03：sync_time —— 校时 + 审计日志都在 worker 段 */
    JOB_QUERY_USERS         /* P7：只读用户清单 —— 读盘在 ② worker，主线程零阻塞 */
} rpc_job_kind_t;

/* query_users 响应缓冲：**必须 ≤ 上行 PAYLOAD_MAX(512)**，否则 cmdq_push 的
 * snprintf 会把它截半截 —— 那正是本指令要消灭的「静默截断」。取 480 留余量。 */
#define RPC_Q_USERS_BUF 480

typedef struct {
    rpc_job_kind_t kind;
    char  req_id[32];
    int   code;          /* ② 写，③ 读 */
    char  msg[80];       /* ② 写，③ 读 */
    bool  has_otp;
    char  otp[32];       /* 主线程按值拷入，② 在 worker 校验 */

    /* ★ 安全红线（面试会追问）：明文 PIN 在 job 里短暂驻留。
     *   ② 中 pin_hash 之后**立刻** memset 清零；③ 的 rpc_job_done 在 free 前
     *   **再清一次**。绝不能只靠 free —— free 不擦内容，PIN 会以明文形式
     *   留在堆里直到该块被复用（core dump / 堆越界读都会泄出来）。 */
    char  pin[64];

    safe_user_t   u;     /* JOB_ADD_USER / JOB_SET_FACE_ENABLE */
    int     del_uid;
    int32_t del_fid;     /* ② 写（JOB_DEL_USER 回填），③ 读 */
    int     face_otp_after;
    int     face_timeout_s;
    bool    fe_enable;   /* JOB_SET_FACE_ENABLE：启用标志（按值，避免被 user_find 覆盖） */
    bool    unlock_ok;   /* ② 写（JOB_REMOTE_UNLOCK），③ 读 */
    int64_t target_ts;   /* JOB_SET_TIME：目标 Unix 秒 */
    char    inj_detail[96];  /* JOB_INJECT_FACE_LOG */

    int     q_offset;           /* JOB_QUERY_USERS：起始下标（分页游标，按值拷入） */
    char    q_buf[RPC_Q_USERS_BUF]; /* ② 拼好的完整响应（含信封），③ 原样上行 */
} rpc_job_t;

/* JSON 字符串内容转义（仅 " 和 \，控制字符在本工程字段里不会出现：
 * 用户名限字母数字、req_id 是对端自拟的短标识）。正常路径（字母数字）零改动。 */
static void rpc_json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (in == NULL) { out[0] = '\0'; return; }
    for (size_t i = 0; in[i] != '\0' && o + 3 <= cap - 1; i++) {
        if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

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
        /* ★ QA-21：PBKDF2 下沉 worker。PC 上几十 ms、A7 上几百 ms 的纯 CPU 热点，
         *   放在主线程会直接卡死 20ms 泵（单拍预算防不住单条指令内部的耗时）。 */
        uint8_t salt[16];
        if (pin_hash(j->pin, salt, j->u.pin_hash) != 0) {
            j->code = 1003;
            snprintf(j->msg, sizeof(j->msg), "pin 哈希失败");
            memset(j->pin, 0, sizeof(j->pin));   /* 用完即清，不留明文 */
            break;
        }
        /* 明文 PIN 在此之后不再被使用 —— 立刻清零（不能等到 free）。 */
        memset(j->pin, 0, sizeof(j->pin));
        rpc_salt_to_hex(salt, j->u.pin_salt);

        if (j->u.id <= 0) j->u.id = user_next_id();   /* 分配 id 在 worker（读盘）做 */
        int r = user_add(&j->u);
        if (r == 0) {
            /* ★ 远程路径与 UI 路径**必须**留同样的痕：安全设备上「谁在什么时候
             *   动了用户表」，本地 safe.log 与远端 safe/log 看到的必须是同一个
             *   事实 —— 只写本地，远端只能靠 user_count 跳变猜；只上报不落盘，
             *   断网时段的操作就凭空消失。两条都做、失败互不影响。 */
            log_append("user_add", "remote", 1, j->u.name);
            astore_report_user_event("user_add", "remote", 1, j->u.name);
        }
        j->code = r == 0 ? 0 : 1003;
        snprintf(j->msg, sizeof(j->msg),
                 r == 0 ? "ok" : (r == -2 ? "用户名已存在" : "add failed"));
        break;
    }
    case JOB_DEL_USER: {
        /* ★ 审计必须先取名字再删：删完就查不到了，而「删的是谁」正是审计
         *   要回答的问题。此前远程删除完全无痕（本地 safe.log 与远端 safe/log
         *   双缺失）—— 事后无法追责/对账，且 fd85ae1 只声明了「远程改用户
         *   不做」这个边界，从未声明「远程删用户免审计」；与 UI 删除路径
         *   （del_worker 两条都写）不一致。远程路径同样要与 UI 对齐：
         *   安全设备上「谁能删用户」，本地和远端看到的必须是同一个事实。 */
        safe_user_t victim;
        char vname[32] = "-";
        if (user_find_by_id(j->del_uid, &victim) == 0)
            strncpy(vname, victim.name, sizeof(vname) - 1);
        int r = user_del_cascade(j->del_uid, &j->del_fid);
        if (r == 0) {
            log_append("user_del", "remote", 1, vname);
            astore_report_user_event("user_del", "remote", 1, vname);
        }
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
        /* ★ 与 UI 启停路径对齐：face_enable 是用户数据的一部分，UI 侧 tog_worker
         *   走 "user_modify" 且两条都写；远程只改的是人脸这一项，evt 仍用
         *   user_modify，detail 写清楚改的是哪一项。 */
        char fdet[72];
        snprintf(fdet, sizeof(fdet), "%s face_enable=%s",
                 j->u.name, j->u.face_enable ? "enabled" : "disabled");
        log_append("user_modify", "remote", 1, fdet);
        astore_report_user_event("user_modify", "remote", 1, fdet);
        j->code = 0; snprintf(j->msg, sizeof(j->msg), "ok");
        break;
    }
    case JOB_SET_FACE_POLICY: {
        if (j->face_otp_after >= 1 || j->face_timeout_s >= 3) {
            user_policy_set_face(j->face_otp_after, j->face_timeout_s);
            /* ★ 全局策略变更同样留痕。只写本地审计、**不发** EV_USER_CHANGED：
             *   该事件语义是「用户数据变了」（订阅方是用户页刷新 + 远程上报），
             *   全局策略不属于用户数据；硬塞进去会让远端收到语义错误的
             *   user_modify 事件。策略变更的可观测走本地 setting_change +
             *   周期性 query_status（策略值本来就随状态上报暴露）。 */
            char pdet[72];
            snprintf(pdet, sizeof(pdet), "face_otp_after=%d face_timeout_s=%d",
                     j->face_otp_after, j->face_timeout_s);
            log_append("setting_change", "remote", 1, pdet);
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
    case JOB_SET_TIME: {
        /* QA-03 第 3 层：改信任根**必须**留痕。此前 sync_time 完全没有审计日志，
         * 安全设备改时钟不留痕是硬伤 —— 拨回过去即可解除锁定、复活过期授权。 */
        int64_t old_ts = (int64_t)hal_time();
        hal_time_set((uint32_t)j->target_ts);
        char detail[96];
        snprintf(detail, sizeof(detail), "old=%lld new=%lld delta=%lld",
                 (long long)old_ts,
                 (long long)j->target_ts,
                 (long long)(j->target_ts - old_ts));
        log_append("time_sync", "remote", 1, detail);
        j->code = 0;
        snprintf(j->msg, sizeof(j->msg), "ok");
        break;
    }
    case JOB_QUERY_USERS: {
        /* ★ 安全红线：只回 id / name / role / enabled / face（是否已录人脸）。
         *   **绝不回 pin_hash / pin_salt / face_id（模组模板号）** —— 本指令是给
         *   远端做「用户数对账」用的，不是同步凭据；凭据字段哪怕只是哈希也不该
         *   出设备（哈希可离线爆破，模板号可被用于定位/伪造）。
         *
         * ★ 为什么分页而不是一次全量：上行 PAYLOAD_MAX=512 是硬上限；静默截断
         *   （截到哪算哪、不标注）会让远端把半份清单当全量去做对账 —— 比不给
         *   数据更糟。所以放不下的条目**整条**不写（绝不截半条），并显式带上
         *   total / count / next_offset / truncated；远端拿 next_offset 再查一次。
         *   宁可多一次往返，不给假数据。
         *
         * ★ 读盘在 ② worker 段（QA-20/QA-21 同款理由）：user_load_all 要解析
         *   整份 users.json，放主线程会卡 LVGL 泵一拍。 */
        safe_user_t *us = NULL;
        int n = 0;
        if (user_load_all(&us, &n) != 0) {
            j->code = RPC_CODE_CMD_FAIL;
            snprintf(j->msg, sizeof(j->msg), "读用户表失败");
            break;
        }
        int off = j->q_offset;
        if (off < 0) off = 0;
        if (off > n) off = n;

        char rid[RPC_REQ_ID_CAP * 2];
        rpc_json_escape(j->req_id, rid, sizeof(rid));
        size_t used = (size_t)snprintf(j->q_buf, sizeof(j->q_buf),
            "{\"ts\":%lld,\"code\":0,\"req_id\":\"%s\",\"total\":%d,\"users\":[",
            (long long)hal_time(), rid, n);
        int cnt = 0;
        for (int i = off; i < n; i++) {
            char nm[sizeof(us[i].name) * 2];
            rpc_json_escape(us[i].name, nm, sizeof(nm));
            char ent[224];
            int el = snprintf(ent, sizeof(ent),
                "%s{\"id\":%d,\"name\":\"%s\",\"role\":\"%s\",\"enabled\":%s,\"face\":%s}",
                cnt ? "," : "", us[i].id, nm, us[i].role,
                us[i].enabled ? "true" : "false",
                us[i].face_id >= 0 ? "true" : "false");
            /* 预留结尾 "],count…}"（≈56B）：放不下整条就停，绝不写半条 */
            if (used + (size_t)el + 56 >= sizeof(j->q_buf)) break;
            memcpy(j->q_buf + used, ent, (size_t)el);
            used += (size_t)el;
            cnt++;
        }
        int next = off + cnt;
        snprintf(j->q_buf + used, sizeof(j->q_buf) - used,
                 "],\"count\":%d,\"next_offset\":%d,\"truncated\":%s}",
                 cnt, next, (next < n) ? "true" : "false");
        j->code = 0;
        snprintf(j->msg, sizeof(j->msg), "ok");
        user_list_free(us);
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
    case JOB_SET_TIME:
        ack(j->req_id, j->code, j->msg);
        break;
    case JOB_QUERY_USERS:
        /* 响应已在 ② 拼好（含信封，≤ PAYLOAD_MAX），这里原样上行；
         * 只有读盘失败才会走到文本 ack。 */
        if (j->q_buf[0] != '\0') mqtt_publish("safe/log", j->q_buf, 1, 0);
        else ack(j->req_id, j->code, j->msg);
        break;
    }
    /* ★ 明文敏感数据：free 之前再清一次（见 rpc_job_t 的注释）。
     * worker 段已清过 pin，但「哈希失败提前 break」等分支也要覆盖；
     * otp 一直在 worker 段被 backend_admin_verify_totp 消费，同样在此统一清。 */
    memset(j->pin, 0, sizeof(j->pin));
    memset(j->otp, 0, sizeof(j->otp));
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

    /* ---- query_status：立即回状态（只读，主线程） ----
     * ★ QA-21：user_count 取 store_cache 快照，**不在主线程读盘**。 */
    if (strcmp(c, "query_status") == 0) {
        int user_count = 0;
        store_cache_user_count(&user_count, NULL);
        char *s = build_status(0, NULL, req_id, user_count);
        if (s) { mqtt_publish("safe/log", s, 1, 0); free(s); }
        cJSON_Delete(root);
        return;
    }

    /* ---- query_users：只读用户清单（分页；② 读盘在 worker） ----
     * 权限分级说明：本指令**无副作用**（重放无害），但不进 readonly 豁免名单 ——
     * 因为它的**返回内容**本身敏感（用户名枚举）。走敏感档意味着：
     * 配置了 MQTT 凭据时匿名/未鉴权通道一律 1006 拒绝，且必须带唯一 req_id。
     * 宁可让轮询方每次换 req_id，也不给匿名连接枚举用户名的口子。 */
    if (strcmp(c, "query_users") == 0) {
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_QUERY_USERS;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        cJSON *oj = params ? cJSON_GetObjectItem(params, "offset") : NULL;
        j->q_offset = cJSON_IsNumber(oj) ? (int)oj->valuedouble : 0;
        worker_post(rpc_job_fn, j, rpc_job_done);
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
        /* 主线程只按值构建（id 与 PIN 哈希都在 ② worker 段做）。 */
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
        /* ★ QA-21：PBKDF2 **不**在主线程算（PC 几十 ms / A7 几百 ms 的纯 CPU 热点，
         *   会把 20ms 泵卡死一拍）。明文 PIN 按值拷进 job，由 ② worker 段算完即清零。
         *   id 也在 ② 分配。这里只做参数校验与按值拷贝。 */
        strncpy(j->pin, pn->valuestring, sizeof(j->pin) - 1);
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

    /* ---- sync_time（敏感：鉴权 + req_id 去重 + **跳变门控** + 审计日志） ----
     * QA-03 三层防御：
     *   第 1 层 白名单分级（a391fb0 已有）：强制鉴权通道 + req_id 去重；
     *   第 2 层 跳变幅度门控（本次新增）：|target-now| > 3600s 需管理员动态码；
     *   第 3 层 审计日志（本次新增）：每次校时都写 safe.log（此前完全没有留痕）。
     *
     * ★ 时序（避免与 TOTP 的鸡生蛋 / TOCTOU 冲突）：
     *   ① 主线程：先取 now（**在校时之前**）→ 门控判定 → 需要 OTP 就回 2001 让
     *      对端补码；不需要就直接投作业。
     *   ② worker：OTP 校验 → hal_time_set → log_append。「校验」与「校时」在
     *      同一段串行执行，天然避免「校验通过后时钟又被改」的 TOCTOU。 */
    if (strcmp(c, "sync_time") == 0) {
        int64_t target = 0;
        {
            cJSON *tsj = params ? cJSON_GetObjectItem(params, "ts") : NULL;
            if (cJSON_IsNumber(tsj)) target = (int64_t)tsj->valuedouble;
        }
        /* ★ now 必须在任何校时动作之前取，否则门控会被自己绕过。 */
        const int64_t now_ts = (int64_t)hal_time();
        const int gate_t = rpc_sync_time_gate(target, now_ts, false, RPC_CLOCK_JUMP_FREE_SEC);
        if (gate_t == RPC_CODE_BAD_PARAM) {
            ack(req_id, RPC_CODE_BAD_PARAM, "invalid ts（须为正的 Unix 秒）");
            cJSON_Delete(root);
            return;
        }
        char otpbuf[32];
        const bool has_otp = (gate_t == RPC_CODE_NEED_OTP) ? rpc_otp_present(root, otpbuf) : false;
        if (gate_t == RPC_CODE_NEED_OTP && !has_otp) {
            ack(req_id, RPC_CODE_NEED_OTP, "时钟跳变超过 3600s，需要管理员动态码");
            cJSON_Delete(root);
            return;
        }
        rpc_job_t *j = calloc(1, sizeof(*j));
        if (!j) { ack(req_id, 1003, "internal"); cJSON_Delete(root); return; }
        j->kind = JOB_SET_TIME;
        strncpy(j->req_id, req_id, sizeof(j->req_id) - 1);
        j->target_ts = target;
        j->has_otp = has_otp;
        if (has_otp) strncpy(j->otp, otpbuf, sizeof(j->otp) - 1);
        worker_post(rpc_job_fn, j, rpc_job_done);
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
            log_append("inject_face", "remote", 1, detail);  /* GATE-EXEMPT: 仅当 job 分配失败
                                                    *（内存耗尽）时才在主线程写这条审计日志；
                                                    * 正常路径的写盘都在 rpc_job_fn（worker 线程） */
        }
        ack(req_id, 0, "injected");
        cJSON_Delete(root);
        return;
    }

    ack(req_id, 1003, "未知指令");
    cJSON_Delete(root);
}

/* 单拍预算：20ms 主循环里 rpc_poll 最多同步处理这么多条指令、或花这么多毫秒，
 * 取先到；剩余留到下一拍。
 *
 * ★ 这条预算防的是「多条指令累积吃满一拍」，**不是**单条指令的耗时 ——
 *   它是**在两条指令之间**检查的，单条指令内部跑 300ms 它照样拦不住。
 *   单条耗时靠「重活下沉 worker」解决（build_status 的整表读盘 → store_cache
 *   快照；add_user 的 PBKDF2 → rpc_job_fn）。两者不可互相替代，都要有。
 * 8ms ≈ 一拍的 40%，给 LVGL/face 留足余量；8 条覆盖一次下发 + 其后状态轮询。 */
#define RPC_POLL_MAX_MSGS 8
#define RPC_POLL_MAX_MS   8

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

