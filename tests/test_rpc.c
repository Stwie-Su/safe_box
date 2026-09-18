/**
 * @file test_rpc.c
 * rpc_guard（远程指令去重 / 时效 / 命令分级）单元测试。
 *
 * 覆盖测试计划要求的四条主线：
 *   1) 同 req_id 重复被拒；
 *   2) 不同 req_id 通过；
 *   3) ts 过期被拒（含超前方向）；
 *   4) 无 ts 放行（向后兼容）。
 * 另补：环形表容量淘汰、命令分级（含 inject_face 入敏感档 + 调试钩子识别）、
 *       准入策略 rpc_guard_admit（去重=仅敏感档 / 时效=全部档 / 敏感命令需非空 req_id）、
 *       调试钩子门控 rpc_guard_debug_hook_gate（默认关死、开启仍需鉴权）。
 *
 * 只链 core（rpc_guard.c 无第三方依赖），不启 MQTT / 不碰 LVGL。
 */
#include "test_util.h"

#include <string.h>

#include "core/remote/rpc_guard.h"

int main(void)
{
    const int64_t NOW = 1700000000;   /* 固定基准时刻，测试完全确定 */

    /* ---- 1. 同 req_id 重复被拒（防重放） ---- */
    {
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        CHECK(rpc_req_cache_admit(&c, "req-A", NOW) == true);    /* 首次：准予 */
        CHECK(rpc_req_cache_admit(&c, "req-A", NOW) == false);   /* 重放：拒绝 */
        CHECK(rpc_req_cache_admit(&c, "req-A", NOW + 3600) == false); /* 即使隔很久，仍在窗口内被拒 */
    }

    /* ---- 2. 不同 req_id 通过 ---- */
    {
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        CHECK(rpc_req_cache_admit(&c, "req-1", NOW) == true);
        CHECK(rpc_req_cache_admit(&c, "req-2", NOW) == true);
        CHECK(rpc_req_cache_admit(&c, "req-3", NOW) == true);
        /* 与已有 id 仅大小写/后缀不同，也应各自通过 */
        CHECK(rpc_req_cache_admit(&c, "req-1x", NOW) == true);
    }

    /* ---- 3. 时效：过期 / 超前 / 窗内 / 边界 ---- */
    {
        /* 过期：早于窗口上限 */
        CHECK(rpc_ts_fresh(NOW - 121, NOW, RPC_TS_WINDOW_SEC) == false);
        /* 超前：晚于窗口上限（本机时钟慢于对端） */
        CHECK(rpc_ts_fresh(NOW + 121, NOW, RPC_TS_WINDOW_SEC) == false);
        /* 窗内：恰好等于窗口边界视为通过 */
        CHECK(rpc_ts_fresh(NOW - 120, NOW, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_ts_fresh(NOW + 120, NOW, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_ts_fresh(NOW, NOW, RPC_TS_WINDOW_SEC) == true);
        /* 负数/0：视为「未提供」→ 放行，不当作过期 */
        CHECK(rpc_ts_fresh(0, NOW, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_ts_fresh(-5, NOW, RPC_TS_WINDOW_SEC) == true);
    }

    /* ---- 4. 无 ts 放行（向后兼容） ---- */
    {
        /* ts==0 即客户端未带 params.ts，无论本机时间如何都放行 */
        CHECK(rpc_ts_fresh(0, 0, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_ts_fresh(0, NOW + 99999, RPC_TS_WINDOW_SEC) == true);
    }

    /* ---- 5. 空 / NULL req_id 不参与去重 ---- */
    {
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        CHECK(rpc_req_cache_admit(&c, "", NOW) == true);
        CHECK(rpc_req_cache_admit(&c, "", NOW) == true);       /* 两次空串都放行，不互相判重 */
        CHECK(rpc_req_cache_admit(&c, NULL, NOW) == true);
        /* 空串不进表：随后带同一「空」语义之外的 id 仍正常 */
        CHECK(rpc_req_cache_admit(&c, "x", NOW) == true);
    }

    /* ---- 6. 环形表容量淘汰：最旧的一条被挤出后不再算重复 ---- */
    {
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        char id[16];
        for (int i = 0; i < RPC_REQ_CACHE_CAP; i++) {
            snprintf(id, sizeof(id), "r%d", i);
            CHECK(rpc_req_cache_admit(&c, id, NOW) == true);
        }
        /* 此时表已满；再插入一条会覆盖 head 指向的最旧项（r0） */
        CHECK(rpc_req_cache_admit(&c, "r-new", NOW) == true);
        /* 仍在表内的 r1 应被判重（先查，避免被后续插入挤出） */
        CHECK(rpc_req_cache_admit(&c, "r1", NOW) == false);
        /* r0 已被 "r-new" 挤出 → 再次出现应被视为「首次」，放行 */
        CHECK(rpc_req_cache_admit(&c, "r0", NOW) == true);
    }

    /* ---- 7. 命令分级 ---- */
    {
        /* 敏感档（与 rpc.c 的 OTP 要求一致） */
        CHECK(rpc_cmd_is_sensitive("remote_unlock")   == true);
        CHECK(rpc_cmd_is_sensitive("add_user")        == true);
        CHECK(rpc_cmd_is_sensitive("del_user")        == true);
        CHECK(rpc_cmd_is_sensitive("set_face_enable") == true);
        CHECK(rpc_cmd_is_sensitive("set_face_policy") == true);
        CHECK(rpc_cmd_is_sensitive("inject_face")     == true);   /* M2：并入敏感档 */
        /* 只读档：当前仅 query_status（白名单豁免去重） */
        CHECK(rpc_cmd_is_sensitive("query_status") == false);
        CHECK(rpc_cmd_is_readonly("query_status")  == true);
        CHECK(rpc_cmd_is_readonly("del_user")      == false);
        /* ★ 白名单分级（安全方向）：除只读外**一律敏感**。
         * 这把原先被黑名单漏掉的 sync_time / submit_otp 收进敏感档
         * （sync_time 匿名可改设备时钟，而时钟是锁定与 TOTP 的信任根），
         * 未知指令与 NULL 同样按最严处理 —— 漏登记的后果从「重投被二次执行」
         * 变成「回 1004 让对端重试」，失败方向落在安全的一侧。 */
        CHECK(rpc_cmd_is_sensitive("sync_time")    == true);
        CHECK(rpc_cmd_is_sensitive("submit_otp")   == true);
        CHECK(rpc_cmd_is_sensitive("nonsense_cmd") == true);
        CHECK(rpc_cmd_is_sensitive(NULL)           == true);
        /* 调试钩子识别：仅 inject_face */
        CHECK(rpc_cmd_is_debug_hook("inject_face")  == true);
        CHECK(rpc_cmd_is_debug_hook("query_status") == false);
        CHECK(rpc_cmd_is_debug_hook("del_user")     == false);
        CHECK(rpc_cmd_is_debug_hook(NULL)           == false);
    }

    /* ---- 8. 组合语义：一条完整请求 = 时效 + 去重，任一不过即拒 ---- */
    {
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        /* 新鲜 + 首次 → 通过 */
        CHECK(rpc_ts_fresh(NOW, NOW, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_req_cache_admit(&c, "req-Z", NOW) == true);
        /* 同 id 重放（即使 ts 新鲜）→ 去重拒绝 */
        CHECK(rpc_ts_fresh(NOW, NOW, RPC_TS_WINDOW_SEC) == true);
        CHECK(rpc_req_cache_admit(&c, "req-Z", NOW) == false);
    }

    /* ---- 9. 准入策略（rpc_guard_admit）：去重=仅敏感档；时效=全部档 ----
     * 本节锁定本次定论的核心语义，是回归防线，勿删。 */
    {
        rpc_req_cache_t c;

        /* 9a. 只读命令：同 req_id 重复发送 → **放行**（去重豁免）。
         *     理由：防「手机端/验证脚本用固定 req_id 反复查状态」白吃 1004。 */
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "query_status", "poll-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
        CHECK(rpc_guard_admit(&c, "query_status", "poll-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
        CHECK(rpc_guard_admit(&c, "query_status", "poll-1", 0,   NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);

        /* 9b. 敏感命令：同 req_id 重复 → **第二次拒绝**（防重放）；换 id 放行。 */
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "del_user", "s-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
        CHECK(rpc_guard_admit(&c, "del_user", "s-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_DUPLICATE);
        CHECK(rpc_guard_admit(&c, "del_user", "s-2", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);

        /* 9c. 时效对所有档生效：只读 / 敏感 带过期 ts 都回 1005；不带 ts（=0）放行。 */
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "query_status", "p", NOW - 500, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_STALE_TS);
        CHECK(rpc_guard_admit(&c, "del_user",     "d", NOW - 500, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_STALE_TS);
        CHECK(rpc_guard_admit(&c, "del_user",     "d", 0,         NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);

        /* 9d. sync_time 例外：其 ts 是「待设置时间」而非请求时间戳，不校时效。 */
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "sync_time", "t1", NOW + 999999, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);

        /* 9e. L1：**敏感命令必须带非空 req_id**（否则去重/防重放无从建立）。
         *     敏感 + 空 id → 拒绝 1008；只读 + 空 id → 仍放行（豁免语义保留）。 */
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "add_user",     "", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_MISSING_REQ_ID);
        CHECK(rpc_guard_admit(&c, "del_user",     "", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_MISSING_REQ_ID);
        CHECK(rpc_guard_admit(&c, "inject_face",  "", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_MISSING_REQ_ID);
        CHECK(rpc_guard_admit(&c, "query_status", "", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
        /* 带非空 id 的敏感命令则正常放行 */
        CHECK(rpc_guard_admit(&c, "add_user", "u-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
    }

    /* ---- 10. M2：调试钩子 inject_face 门控（默认关死；开启仍需鉴权） ---- */
    {
        /* 未启用：任何来源都拒绝 → 1007 */
        CHECK(rpc_guard_debug_hook_gate(false, false) == RPC_CODE_DEBUG_DISABLED);
        CHECK(rpc_guard_debug_hook_gate(false, true)  == RPC_CODE_DEBUG_DISABLED);
        /* 已启用但通道未鉴权 → 1006 */
        CHECK(rpc_guard_debug_hook_gate(true,  false) == RPC_CODE_UNAUTHORIZED);
        /* 已启用且已鉴权 → 放行 */
        CHECK(rpc_guard_debug_hook_gate(true,  true)  == RPC_CODE_OK);

        /* inject_face 已并入敏感档 ⇒ 同 req_id 重复也会被去重拦（1004）。 */
        rpc_req_cache_t c;
        rpc_req_cache_init(&c);
        CHECK(rpc_guard_admit(&c, "inject_face", "inj-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_OK);
        CHECK(rpc_guard_admit(&c, "inject_face", "inj-1", NOW, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_DUPLICATE);
        /* 带过期 ts 也回 1005（时效对敏感档生效） */
        CHECK(rpc_guard_admit(&c, "inject_face", "inj-2", NOW - 500, NOW, RPC_TS_WINDOW_SEC) == RPC_CODE_STALE_TS);
    }

    /* ---- 11. env 开关白名单解析（rpc_env_flag_on）：默认方向=关 ---- */
    {
        /* 开启：仅明确肯定值，大小写不敏感 */
        CHECK(rpc_env_flag_on("1")    == true);
        CHECK(rpc_env_flag_on("true") == true);
        CHECK(rpc_env_flag_on("TRUE") == true);
        CHECK(rpc_env_flag_on("Yes")  == true);
        CHECK(rpc_env_flag_on("on")   == true);
        CHECK(rpc_env_flag_on("On")   == true);
        /* 关闭：其余一切（含脚枪 "false"/"no"、"0"、空、NULL、乱写）都必须落到关 */
        CHECK(rpc_env_flag_on("0")     == false);
        CHECK(rpc_env_flag_on("false") == false);
        CHECK(rpc_env_flag_on("FALSE") == false);
        CHECK(rpc_env_flag_on("no")    == false);
        CHECK(rpc_env_flag_on("off")   == false);
        CHECK(rpc_env_flag_on("")      == false);
        CHECK(rpc_env_flag_on(NULL)    == false);
        CHECK(rpc_env_flag_on("2")     == false);
        CHECK(rpc_env_flag_on("yesplease") == false);   /* 必须完整匹配 */
        CHECK(rpc_env_flag_on(" true")     == false);    /* 不 trim：保守关闭 */
    }

    TEST_RESULT();
}
