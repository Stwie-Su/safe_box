/**
 * @file rpc_guard.c
 * rpc_guard.h 的实现。纯逻辑、零依赖、无全局状态（便于单测）。
 */
#include "core/remote/rpc_guard.h"

#include <string.h>

void rpc_req_cache_init(rpc_req_cache_t *cache)
{
    if (cache == NULL) return;
    memset(cache, 0, sizeof(*cache));
    cache->head  = 0;
    cache->count = 0;
}

bool rpc_req_cache_admit(rpc_req_cache_t *cache, const char *req_id, int64_t now_sec)
{
    /* 无缓存或无 req_id：不去重（放行）。空 req_id 一律放行是刻意的——
     * 否则一批「都没带 req_id」的请求会被彼此判重而全部被拒。 */
    if (cache == NULL || req_id == NULL || req_id[0] == '\0') return true;

    /* 线性查重：容量仅 32，比哈希表更省事、无碰撞、无动态分配。
     * 只比较前 RPC_REQ_ID_CAP-1 个字符，与写入时的截断长度一致。 */
    for (int i = 0; i < cache->count; i++) {
        if (strncmp(cache->slots[i].req_id, req_id, RPC_REQ_ID_CAP - 1) == 0) {
            return false;   /* 命中：窗口内已见过 → 判为重复 */
        }
    }

    /* 未命中：记入环形表（覆盖最旧的一条） */
    rpc_req_slot_t *slot = &cache->slots[cache->head];
    strncpy(slot->req_id, req_id, RPC_REQ_ID_CAP - 1);
    slot->req_id[RPC_REQ_ID_CAP - 1] = '\0';
    slot->ts_seen = now_sec;

    cache->head = (cache->head + 1) % RPC_REQ_CACHE_CAP;
    if (cache->count < RPC_REQ_CACHE_CAP) cache->count++;
    return true;
}

bool rpc_ts_fresh(int64_t ts, int64_t now_sec, int window_sec)
{
    if (ts <= 0) return true;          /* 未提供时间戳：向后兼容放行（见头注释） */
    if (window_sec < 0) window_sec = 0;

    int64_t delta = now_sec - ts;
    if (delta < 0) delta = -delta;     /* 滞后（过期）与超前（时钟漂移）同为超窗 */
    return delta <= (int64_t)window_sec;
}

bool rpc_cmd_is_sensitive(const char *cmd)
{
    if (cmd == NULL) return false;
    return strcmp(cmd, "remote_unlock")   == 0
        || strcmp(cmd, "add_user")        == 0
        || strcmp(cmd, "del_user")        == 0
        || strcmp(cmd, "set_face_enable") == 0
        || strcmp(cmd, "set_face_policy") == 0
        /* inject_face 能合成 FACE_RES_OK 走 do_unlock（= 伪造开锁结果），
         * 是「有副作用/能伪造安全结果」的命令，必须与其它敏感指令同级门控。 */
        || strcmp(cmd, "inject_face")     == 0;
}

bool rpc_cmd_is_debug_hook(const char *cmd)
{
    return cmd != NULL && strcmp(cmd, "inject_face") == 0;
}

int rpc_guard_debug_hook_gate(bool allow, bool authenticated)
{
    if (!allow)        return RPC_CODE_DEBUG_DISABLED;   /* 默认关死：未显式开启一律拒绝 */
    if (!authenticated) return RPC_CODE_UNAUTHORIZED;    /* 开启后仍要求鉴权连接 */
    return RPC_CODE_OK;
}

/* 仅 ASCII 的小写化（不引入 <strings.h>/locale，保证板上/PC 一致） */
static char rpc_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool rpc_streq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;
    while (*a != '\0' && *b != '\0') {
        if (rpc_lower(*a) != rpc_lower(*b)) return false;
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

bool rpc_env_flag_on(const char *value)
{
    if (value == NULL) return false;
    return rpc_streq_ci(value, "1")
        || rpc_streq_ci(value, "true")
        || rpc_streq_ci(value, "yes")
        || rpc_streq_ci(value, "on");
}

int rpc_guard_admit(rpc_req_cache_t *cache, const char *cmd, const char *req_id,
                    int64_t ts, int64_t now_sec, int window_sec)
{
    bool sensitive = rpc_cmd_is_sensitive(cmd);

    /* (0) 敏感档必须带非空 req_id：否则去重/防重放无从建立（L1）。
     *     只读档不受此限（空 req_id 仍放行，保持既有豁免语义）。 */
    if (sensitive && (req_id == NULL || req_id[0] == '\0')) {
        return RPC_CODE_MISSING_REQ_ID;
    }

    /* (1) 时效：所有档生效（ts<=0 即「未提供」时 rpc_ts_fresh 直接放行）。
     *     sync_time 的 params.ts 是「待设置时间」而非请求时间戳，跳过以免误伤。 */
    if (cmd == NULL || strcmp(cmd, "sync_time") != 0) {
        if (!rpc_ts_fresh(ts, now_sec, window_sec)) return RPC_CODE_STALE_TS;
    }

    /* (2) 去重：**仅敏感档**（有副作用）。只读档豁免——重放只读无实际危害。 */
    if (sensitive) {
        if (!rpc_req_cache_admit(cache, req_id, now_sec)) return RPC_CODE_DUPLICATE;
    }

    return RPC_CODE_OK;
}
