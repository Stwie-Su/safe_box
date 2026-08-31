/**
 * @file rpc.c
 * RPC 指令分发实现。指令集与错误码严格对应需求文档 2.6 / 2.7。
 *
 * 线程模型：mqtt_start(..., NULL) 不注册回调，消息进入 mqtt_client 内部队列；
 * rpc_poll() 在主线程（LVGL 主循环 20ms 泵）调用 mqtt_take() 取走并处理。
 */
#include "rpc.h"
#include "mqtt_client.h"
#include "store.h"
#include "totp.h"
#include "hal/hal_time.h"
#include "hal/hal_face.h"
#include "unlock_backend.h"
#include "auth_fsm.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char   g_local_otp_req[32];
static bool   g_local_otp_pending = false;
static rpc_event_hook_t g_evt_hook = NULL;

void rpc_set_event_hook(rpc_event_hook_t hook) { g_evt_hook = hook; }

void rpc_init(const char *host, int port)
{
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
    mqtt_publish("safe/log", s, 1, 0);
    free(s);
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
    cJSON_AddNumberToObject(j, "last_score", auth_fsm_last_score());
    cJSON_AddStringToObject(j, "last_user", auth_fsm_pending_user());
    cJSON_AddNumberToObject(j, "user_count", n);
    cJSON_AddStringToObject(j, "time_src", hal_time_source() == TIME_SRC_RTC ? "rtc" : "sys");
    cJSON_AddNumberToObject(j, "mqtt_online", mqtt_is_connected() ? 1 : 0);
    cJSON_AddStringToObject(j, "fw", "1.0.0");
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

    /* ---- add_user（敏感） ---- */
    if (strcmp(c, "add_user") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        cJSON *nm = params ? cJSON_GetObjectItem(params, "name") : NULL;
        safe_user_t u; memset(&u, 0, sizeof(u));
        u.id = uid > 0 ? uid : user_next_id();
        if (cJSON_IsString(nm) && nm->valuestring) strncpy(u.name, nm->valuestring, sizeof(u.name) - 1);
        u.role[0] = '\0'; strncpy(u.role, "user", sizeof(u.role) - 1);
        u.enabled = true; u.face_id = -1;
        int r = user_add(&u);
        ack(req_id, r == 0 ? 0 : 1003, r == 0 ? "ok" : "add failed");
        cJSON_Delete(root);
        return;
    }

    /* ---- del_user（敏感） ---- */
    if (strcmp(c, "del_user") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int uid = params ? (cJSON_GetObjectItem(params, "uid") ? cJSON_GetObjectItem(params, "uid")->valueint : 0) : 0;
        int r = user_del(uid);
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
        u.face_enable = en ? true : false;
        user_update(&u);
        ack(req_id, 0, "ok");
        cJSON_Delete(root);
        return;
    }

    /* ---- set_threshold（敏感） ---- */
    if (strcmp(c, "set_threshold") == 0) {
        if (!check_otp_required(root, true, req_id)) { cJSON_Delete(root); return; }
        int v = params && cJSON_GetObjectItem(params, "value") ? cJSON_GetObjectItem(params, "value")->valueint : 0;
        if (v >= 60 && v <= 100) user_policy_set_score(v, v - 25);
        ack(req_id, v >= 60 && v <= 100 ? 0 : 1001, "ok");
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

    /* ---- inject_score（调试用，无需 otp） ---- */
    if (strcmp(c, "inject_score") == 0) {
        int score = params && cJSON_GetObjectItem(params, "score") ? cJSON_GetObjectItem(params, "score")->valueint : 0;
        int fid = params && cJSON_GetObjectItem(params, "face_id") ? cJSON_GetObjectItem(params, "face_id")->valueint : hal_face_sim_id();
        hal_face_inject(fid, score);
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
    while (mqtt_take(topic, sizeof(topic), payload, sizeof(payload))) {
        dispatch(payload);
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
