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
     * 只比较前 RPC_REQ_ID_CAP-1 个字符，与写入时的截断长度一致。
     *
     * ★ 为什么**刻意不加 TTL**（曾评估过，最终否决，勿随手改回）：
     *   ① 「灌满 32 条把表冲掉」这个攻击面在**白名单分级**落地后已不成立 ——
     *      能进入本函数的只有敏感档，而敏感档在 rpc.c 里**先过鉴权**
     *      （rpc.c:205 的 rpc_cmd_is_sensitive + channel_authorized），
     *      匿名连接根本走不到这里，无法灌 req_id。
     *   ② 加了 TTL 反而**削弱**防重放：旧 req_id 在 TTL 到期后可再次通过，
     *      而容量仍是 32 条 —— TTL 并不解决容量冲垮，只让久远条目让出槽位。
     *   ③ 因此「同一 req_id 永久判重（只要还在表内）」是更保守的选择。
     * 若将来把去重表容量上调，再重新评估 TTL 的取舍。 */
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

/* 只读指令：重放无副作用，故豁免去重。
 * 当前 rpc.c 共 9 条指令，唯 query_status 是无副作用的查询，其余 8 条
 * （add_user / del_user / remote_unlock / set_face_enable / set_face_policy /
 *   inject_face / submit_otp / sync_time）都会改变状态或影响安全判定。 */
bool rpc_cmd_is_readonly(const char *cmd)
{
    return cmd != NULL && strcmp(cmd, "query_status") == 0;
}

bool rpc_cmd_is_sensitive(const char *cmd)
{
    /* ★ 白名单式：除明确只读的之外，**一律按敏感处理**（含 NULL / 未知指令）。
     * 理由见头文件：黑名单的漏登记后果是「重投被二次执行」，白名单的漏登记
     * 后果只是「回 1004 让对端重试」—— 后者才是安全的失败方向。
     * 这条同时把 submit_otp 与 sync_time 收进敏感档（原先两者都不在其中）。 */
    return !rpc_cmd_is_readonly(cmd);
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
     *     ★ sync_time 保留豁免：它传进来的 ts 语义是「**待设置**的时间」而非
     *       「请求时间戳」，按请求时间戳校验会误伤对时/校时指令（这是语义冲突，
     *       不是疏漏）。它已被白名单分级收进敏感档 —— 强制鉴权 + req_id 去重，
     *       重放会被 1004 拦下，故保留时效豁免的风险可接受。
     *       注：rpc.c 传的是另一个字段 req_ts 作请求时间戳，两者不共用一个值。 */
    if (cmd == NULL || strcmp(cmd, "sync_time") != 0) {
        if (!rpc_ts_fresh(ts, now_sec, window_sec)) return RPC_CODE_STALE_TS;
    }

    /* (2) 去重：**仅敏感档**（有副作用）。只读档豁免——重放只读无实际危害。 */
    if (sensitive) {
        if (!rpc_req_cache_admit(cache, req_id, now_sec)) return RPC_CODE_DUPLICATE;
    }

    return RPC_CODE_OK;
}
