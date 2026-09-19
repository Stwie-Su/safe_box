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

/* 校验管理操作是否需要 otp。返回 true 表示已校验通过（或不需要）。 */
static bool check_otp_required(cJSON *root, bool sensitive, char *req_id)
{
    if (!sensitive) return true;
    cJSON *p = cJSON_GetObjectItem(root, "params");
    cJSON *otp = p ? cJSON_GetObjectItem(p, "otp") : NULL;
    if (!cJSON_IsString(otp) || !otp->valuestring || !*otp->valuestring) {
        ack(req_id, 2001, "需要动态码");
        return false;
    }
    if (backend_admin_verify_totp(otp->valuestring) != AUTH_OK) {
        ack(req_id, 2002, "动态码错误或已使用");
        return false;
    }
    return true;
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
     * 顺序：鉴权 → 时效+去重。任一未过立即回执并 return，绝不进入指令分支——
     * 保证「越权 / 过期 / 重复」的请求不会先改状态再被判定（如先删了用户再判重）。
     * 分级语义（定论）：**去重仅敏感档；时效对所有档（带 ts 时）**。 */
    const int64_t now_sec = (int64_t)hal_time();

    /* (a) 权限分级：敏感指令要求鉴权通道（未配置凭据时保持旧行为，见 channel_authorized）。 */
    if (rpc_cmd_is_sensitive(c) && !channel_authorized()) {
        ack(req_id, RPC_CODE_UNAUTHORIZED,
            "未鉴权通道：敏感指令被拒绝（需配置 MQTT 凭据并以鉴权连接接入）");
        cJSON_Delete(root);
        return;
    }

    /* (a2) 调试钩子（inject_face：能伪造开锁结果）——默认关死，需 env 显式开启；
     *      开启后仍要求**鉴权连接**。放在去重之前，避免给被拒请求占用去重槽。 */
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

    /* (b) 时效（所有档，带 params.ts 时）+ 去重（仅敏感档）：策略集中在 rpc_guard_admit。
     *     只读档豁免去重——重放 query_status 无实际危害，避免固定 req_id 轮询吃 1004。 */
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

    /* ---- query_status：立即回状态（验收用例 1） ---- */
    if (strcmp(c, "query_status") == 0) {
        char *s = build_status(0, NULL, req_id);
        if (s) { mqtt_publish("safe/log", s, 1, 0); free(s); }
        cJSON_Delete(root);
        return;
    }

    /* ---- remote_unlock ---- */
    if (strcmp(c, "remote_unlock") == 0) {
        /* FR-4 / 验收 8.3：锁定期间一律拒绝，包括管理员远程指令（返回 3001）。
         * 先拦截再校验 otp，避免"锁定期间仅凭动态码即可远程开锁"的越权路径。 */
        if (auth_fsm_state() == FSM_LOCKOUT) {
            ack(req_id, 3001, "系统处于锁定状态");
            cJSON_Delete(root);
            return;
        }
        cJSON *otp = params ? cJSON_GetObjectItem(params, "otp") : NULL;
        if (cJSON_IsString(otp) && otp->valuestring && *otp->valuestring) {
            if (backend_admin_verify_totp(otp->valuestring) == AUTH_OK) {
                auth_fsm_note_unlock("admin");
                ack(req_id, 0, "ok");
            } else {
                ack(req_id, 2002, "动态码错误或已使用");
            }
        } else {
            /* ADR-4：无 otp → 触发板子本地弹出动态码页，回 2001 */
            strncpy(g_local_otp_req, req_id, sizeof(g_local_otp_req) - 1);
            g_local_otp_pending = true;
            ack(req_id, 2001, "请在设备输入动态码");
        }
        cJSON_Delete(root);
        return;
    }

    /* ---- add_user（敏感） ----
     * FR-9 创建入口（v1.8）：本指令现在支持 role（admin/user/temp，含临时角色）、
     * pin（必填，只存哈希）、valid_until 与 use_limit（临时授权，缺省 0 = 不限）。
     * 修前行为：role 写死 "user"、完全不设 PIN —— 而 user_add 要求 pin_hash 非空，
     * 因此这条指令**从来就没有成功过**（恒回 1003），临时用户在远程通道也无法创建。 */
    if (strcmp(c, "add_user") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
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
        /* 临时授权字段：缺省 0 表示不限（与 store 的语义一致）。
         * valid_until 用 valuedouble 取，避免 int 取值范围把 2038 年后的时间戳截断。 */
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

        safe_user_t u; memset(&u, 0, sizeof(u));
        u.id = uid > 0 ? uid : user_next_id();
        strncpy(u.name, nm->valuestring, sizeof(u.name) - 1);
        strncpy(u.role, role, sizeof(u.role) - 1);
        strncpy(u.auth_method, "pin", sizeof(u.auth_method) - 1);
        u.enabled = true;
        u.face_id = -1;                 /* 新建用户默认未绑定人脸模板 */
        u.valid_until = valid_until;
        u.use_limit   = use_limit;
        u.used_count  = 0;
        {
            time_t tn = (time_t)hal_time();
            struct tm tmv;
            localtime_r(&tn, &tmv);
            strftime(u.created_at, sizeof(u.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);
        }
        uint8_t salt[16];
        if (pin_hash(pn->valuestring, salt, u.pin_hash) != 0) {
            ack(req_id, 1003, "pin 哈希失败"); cJSON_Delete(root); return;
        }
        rpc_salt_to_hex(salt, u.pin_salt);
        int r = user_add(&u);
        ack(req_id, r == 0 ? 0 : 1003,
            r == 0 ? "ok" : (r == -2 ? "用户名已存在" : "add failed"));
        if (r == 0) log_append("user_add", "remote", 1, u.name);
        cJSON_Delete(root);
        return;
    }

    /* ---- del_user（敏感） ---- */
    if (strcmp(c, "del_user") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        int32_t fid_rm = -1;
        int r = user_del_cascade(uid, &fid_rm);
        /* 本地删成功且归属核验通过 -> 异步删模组模板。若模组忙（SAFE_ERR_BUSY）
         * 会留孤儿模板，由 cred_reconcile 启动对账标失效，不影响本地凭据。 */
        if (r == 0 && fid_rm >= 0) face_service_delete_async(fid_rm);
        ack(req_id, r == 0 ? 0 : 2003, r == 0 ? "ok" : "user not found");
        cJSON_Delete(root);
        return;
    }

    /* ---- set_face_enable（敏感） ---- */
    if (strcmp(c, "set_face_enable") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        int en = params && cJSON_GetObjectItem(params, "enable") ? cJSON_GetObjectItem(params, "enable")->valueint : 0;
        safe_user_t u; memset(&u, 0, sizeof(u));
        if (user_find_by_id(uid, &u) != 0) { ack(req_id, 2003, "user not found"); cJSON_Delete(root); return; }
        /* 不变式：face_enable == true ⟹ face_id >= 0。未绑定人脸（face_id < 0）时拒绝
         * 「启用」——放行就等于主动制造 face_enable=true 而 face_id=-1 的不一致态
         * （该用户刷脸永不命中、被判「未注册人脸」记失败，界面也误显示已启用）。
         * 关闭（en=false）任何情况都允许，用于停用「有绑定但暂不用」的通道。 */
        if (en && u.face_id < 0) {
            ack(req_id, 1001, "该用户未绑定人脸，无法启用人脸通道");
            cJSON_Delete(root);
            return;
        }
        u.face_enable = en ? true : false;
        user_update(&u);
        ack(req_id, 0, "ok");
        cJSON_Delete(root);
        return;
    }

    /* ---- set_face_policy（敏感，取代旧 set_threshold） ---- */
    if (strcmp(c, "set_face_policy") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int otp_after = params && cJSON_GetObjectItem(params, "face_otp_after")
                        ? cJSON_GetObjectItem(params, "face_otp_after")->valueint : 0;
        int timeout   = params && cJSON_GetObjectItem(params, "face_verify_timeout_s")
                        ? cJSON_GetObjectItem(params, "face_verify_timeout_s")->valueint : 0;
        if (otp_after >= 1 || timeout >= 3) {
            user_policy_set_face(otp_after, timeout);
            ack(req_id, 0, "ok");
        } else {
            ack(req_id, 1001, "缺少有效参数");
        }
        cJSON_Delete(root);
        return;
    }

    /* ---- sync_time ---- */
    if (strcmp(c, "sync_time") == 0) {
        int ts = params && cJSON_GetObjectItem(params, "ts") ? cJSON_GetObjectItem(params, "ts")->valueint : 0;
        if (ts > 0) { hal_time_set((uint32_t)ts); ack(req_id, 0, "ok"); }
        else ack(req_id, 1001, "invalid ts");
        cJSON_Delete(root);
        return;
    }

    /* ---- submit_otp（调试用，受 WAIT_OTP 门控） ----
     * 验收 FR-3 用户动态码二次确认路径：仅在状态机处于 WAIT_OTP 时生效，
     * 校验「待确认用户自身」的 TOTP（backend_verify_totp 内部 ±1 窗口 + 一码一用防重放）。
     * 非 WAIT_OTP 态调用会被 FSM 直接忽略（不触发迁移），属于安全可控的调试指令。 */
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

    /* ---- inject_face（调试钩子，取代旧 inject_score）----
     * 门控见上方 (a2)：默认关死（1007），开启后仍需鉴权连接（1006）；已并入敏感档，
     * 故同样吃 req_id 去重与时效。 */
    if (strcmp(c, "inject_face") == 0) {
        cJSON *rs = params ? cJSON_GetObjectItem(params, "reason") : NULL;
        int fid = params && cJSON_GetObjectItem(params, "face_id") ? cJSON_GetObjectItem(params, "face_id")->valueint : -1;
        /* -1 哨兵必须落在**有符号**变量里（D9）：face_reason_t 的取值全为非负，
         * 编译器可以给它选无符号底层类型，此时 `reason < 0` 因整型提升恒为假，
         * 非法 reason 会一路走到注入并回 code=0 假成功。先用 int 接住再做范围校验。 */
        int reason_i = parse_face_reason(cJSON_IsString(rs) && rs->valuestring ? rs->valuestring : "");
        if (reason_i < 0) {
            ack(req_id, 1001, "invalid reason（合法：ok/no_match/liveness_fail/timeout/error）");
        } else {
            face_service_inject(fid, (face_reason_t)reason_i);
            /* 审计：调试钩子每次注入都要留痕（谁/何时/几号模板/什么结果）。
             * log_append 内部带时间戳；操作者记为 "remote"（无 TLS 时无法更强归属）。 */
            {
                char detail[96];
                snprintf(detail, sizeof(detail), "face_id=%d reason=%.32s",
                         fid, cJSON_IsString(rs) && rs->valuestring ? rs->valuestring : "?");
                log_append("inject_face", "remote", 1, detail);
            }
            ack(req_id, 0, "injected");
        }
        cJSON_Delete(root);
        return;
    }

    ack(req_id, 1003, "未知指令");
    cJSON_Delete(root);
}

/* 单拍预算：20ms 主循环里 rpc_poll 最多同步处理这么多条指令、或花这么多毫秒，
 * 取先到；剩余留到下一拍。否则 PBKDF2 / 整文件 IO 会卡住主线程导致触摸失响应。
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

