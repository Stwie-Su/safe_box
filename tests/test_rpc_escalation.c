/**
 * @file test_rpc_escalation.c
 * rpc_guard 的「提权门控」子集单测（QA 补缺，2026-10-06）。
 *
 * 锁定此前**零覆盖**的安全语义：
 *   1) rpc_cmd_needs_otp  —— 白名单式「接管级操作须管理员动态码」判定。
 *      对应 §10 集成验收「接管级操作仍要求动态码」与 FR-29「升级属接管级操作、
 *      须管理员动态码确认」的可执行判据。
 *   2) rpc_sync_time_gate —— sync_time 的时钟跳变门控（QA-03 第 2 层防御）。
 *   3) rpc_cmd_is_known / rpc_cmd_otp_self_escalating —— 已实现指令清单与
 *      「自带补码流程」豁免名单。后者是三份 OTP 判定里唯一
 *      **漏登记即不安全**的一份（见第 9 / 10 节注）。
 *
 * 为什么独立成文件而不并进 test_rpc.c：test_rpc.c 锁定的是「去重 / 时效 /
 * 命令分级」这组语义，本文件锁定的是「提权」这组语义；两者互不依赖，分开后
 * 任一侧语义变更都能定位到具体文件，回归信息不互相稀释。
 *
 * 纯逻辑、无网络、无时钟依赖（时刻由常量给出，结果完全确定）。
 */
#include "test_util.h"

#include <string.h>

#include "core/remote/rpc_guard.h"

int main(void)
{
    const int64_t NOW = 1700000000;   /* 固定基准时刻，测试完全确定 */
    const int64_t MJ  = RPC_CLOCK_JUMP_FREE_SEC;   /* 3600：免动态码跳变上限 */

    /* ---- 1. rpc_cmd_needs_otp：豁免名单**仅** query_status / submit_otp ---- */
    {
        CHECK(rpc_cmd_needs_otp("query_status") == false);
        CHECK(rpc_cmd_needs_otp("submit_otp")   == false);

        /* 其余全部需要动态码（漏登记 = 需要，失败方向落在安全的一侧） */
        CHECK(rpc_cmd_needs_otp("add_user")        == true);
        CHECK(rpc_cmd_needs_otp("del_user")        == true);
        CHECK(rpc_cmd_needs_otp("remote_unlock")   == true);
        CHECK(rpc_cmd_needs_otp("set_face_enable") == true);
        CHECK(rpc_cmd_needs_otp("set_face_policy") == true);
        CHECK(rpc_cmd_needs_otp("inject_face")     == true);
        CHECK(rpc_cmd_needs_otp("sync_time")       == true);
        /* 未知指令 / 空串 / NULL：一律需要 */
        CHECK(rpc_cmd_needs_otp("nonsense_cmd") == true);
        CHECK(rpc_cmd_needs_otp("")             == true);
        CHECK(rpc_cmd_needs_otp(NULL)           == true);
        /* 大小写敏感：豁免名单不得因大小写意外放大 */
        CHECK(rpc_cmd_needs_otp("Query_Status") == true);
        CHECK(rpc_cmd_needs_otp("SUBMIT_OTP")   == true);
        /* 前缀 / 后缀不得命中豁免（必须完整匹配） */
        CHECK(rpc_cmd_needs_otp("query_status2") == true);
        CHECK(rpc_cmd_needs_otp("xsubmit_otp")   == true);
        CHECK(rpc_cmd_needs_otp(" submit_otp")   == true);
    }

    /* ---- 2. 分级正交性：「敏感档」与「需要动态码」是两条独立判据 ---- */
    {
        /* query_status：既不敏感，也不需要 OTP */
        CHECK(rpc_cmd_is_sensitive("query_status") == false);
        CHECK(rpc_cmd_needs_otp("query_status")    == false);
        /* submit_otp：敏感（要鉴权 + 去重），但**不**需要 OTP（否则鸡生蛋） */
        CHECK(rpc_cmd_is_sensitive("submit_otp") == true);
        CHECK(rpc_cmd_needs_otp("submit_otp")    == false);
        /* sync_time：既敏感也需要 OTP（时钟是锁定与 TOTP 的信任根） */
        CHECK(rpc_cmd_is_sensitive("sync_time") == true);
        CHECK(rpc_cmd_needs_otp("sync_time")    == true);
    }

    /* ---- 3. rpc_sync_time_gate：非法目标时间一律不受理 ---- */
    {
        CHECK(rpc_sync_time_gate(0,  NOW, false, MJ) == RPC_CODE_BAD_PARAM);
        CHECK(rpc_sync_time_gate(-1, NOW, false, MJ) == RPC_CODE_BAD_PARAM);
        CHECK(rpc_sync_time_gate(-86400, NOW, false, MJ) == RPC_CODE_BAD_PARAM);
        /* 即便带了动态码，非法时间戳也不受理（不接受「把时间拨回 1970」） */
        CHECK(rpc_sync_time_gate(0,  NOW, true,  MJ) == RPC_CODE_BAD_PARAM);
        CHECK(rpc_sync_time_gate(-1, NOW, true,  MJ) == RPC_CODE_BAD_PARAM);
    }

    /* ---- 4. 常规校时：跳变在免 OTP 门限内（含边界相等） ---- */
    {
        CHECK(rpc_sync_time_gate(NOW,     NOW, false, MJ) == RPC_CODE_OK);  /* 零跳变 */
        CHECK(rpc_sync_time_gate(NOW + 1, NOW, false, MJ) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW - 1, NOW, false, MJ) == RPC_CODE_OK);
        /* 边界相等：|delta| == max_jump 仍属常规校时，不要求动态码 */
        CHECK(rpc_sync_time_gate(NOW + MJ, NOW, false, MJ) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW - MJ, NOW, false, MJ) == RPC_CODE_OK);
    }

    /* ---- 5. 信任根级跳变：超门限必须动态码（前拨与后拨同等危险） ---- */
    {
        CHECK(rpc_sync_time_gate(NOW + MJ + 1, NOW, false, MJ) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW - MJ - 1, NOW, false, MJ) == RPC_CODE_NEED_OTP);
        /* 带了（且已校验通过的）动态码即放行 */
        CHECK(rpc_sync_time_gate(NOW + MJ + 1, NOW, true,  MJ) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW - MJ - 1, NOW, true,  MJ) == RPC_CODE_OK);
        /* 极端跳变（一次拨数年）同样拦下 */
        CHECK(rpc_sync_time_gate(NOW + 3650 * 86400, NOW, false, MJ) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW - 3650 * 86400, NOW, false, MJ) == RPC_CODE_NEED_OTP);
        /* 门限内的常规校时（NTP / RTC 补偿，半小时级）不该打扰用户 */
        CHECK(rpc_sync_time_gate(NOW + 1800, NOW, false, MJ) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW - 1800, NOW, false, MJ) == RPC_CODE_OK);
        /* ★ 门限是 3600s（1 小时）而**不是**一天：半天级跳变同样要动态码。
         *   这条特意钉在这里 —— 很容易被误读成「日漂移免检」。 */
        CHECK(rpc_sync_time_gate(NOW + 86400 / 2, NOW, false, MJ) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW + 86400 / 2, NOW, true,  MJ) == RPC_CODE_OK);
    }

    /* ---- 6. max_jump 边界：0 与负值（负值必须钳到 0，不得反向放大） ---- */
    {
        CHECK(rpc_sync_time_gate(NOW,     NOW, false, 0) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW + 1, NOW, false, 0) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW - 1, NOW, false, 0) == RPC_CODE_NEED_OTP);
        /* 负值钳到 0（若未钳制，-5 会让任何跳变都放行，是失败方向的反转） */
        CHECK(rpc_sync_time_gate(NOW,     NOW, false, -5) == RPC_CODE_OK);
        CHECK(rpc_sync_time_gate(NOW + 1, NOW, false, -5) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW - 1, NOW, false, -5) == RPC_CODE_NEED_OTP);
        CHECK(rpc_sync_time_gate(NOW + 1, NOW, true,  -5) == RPC_CODE_OK);
    }

    /* ---- 7. 契约：门控返回值就是 rpc.c 直接回给对端的回执码 ---- */
    {
        const int g1 = rpc_sync_time_gate(NOW + 100000, NOW, false, MJ);
        const int g2 = rpc_sync_time_gate(NOW + 100000, NOW, true,  MJ);
        CHECK(g1 == RPC_CODE_NEED_OTP);
        CHECK(g2 == RPC_CODE_OK);
        /* 回执码编号是线上的契约，变更会打破手机端脚本 */
        CHECK(RPC_CODE_BAD_PARAM == 1001);
        CHECK(RPC_CODE_NEED_OTP  == 2001);
        CHECK(RPC_CODE_OK        == 0);
    }

    /* ---- 8. rpc_cmd_is_known：已实现指令清单 ----
     * 这张表是「支持哪些指令」的**第二处**表达（另一处是 dispatch 的 strcmp 分支），
     * 靠人同步必漏，故另有 tools/check_rpc_cmd_table.sh 做机械比对（已进 ctest）。
     * 本用例从**语义**侧锁住它：清单内的必须全中，清单外的必须不中。 */
    {
        static const char * const known[] = {
            "query_status", "query_users", "remote_unlock", "add_user", "del_user",
            "set_face_enable", "set_face_policy", "sync_time", "submit_otp", "inject_face",
            "ota_update",
        };
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            CHECK(rpc_cmd_is_known(known[i]) == true);
        }

        /* 未登记 / 空 / NULL：一律 false。
         * ★ 语义：未知指令由 dispatch 回 **1003 未知指令**，不得被入口的 OTP 门控
         *   先拦成 2001 —— 规约（技术路线规约 错误码表）把 1003 定为「未知指令」，
         *   改了就违反对端可见的错误码约定。 */
        CHECK(rpc_cmd_is_known("nonsense_cmd") == false);
        CHECK(rpc_cmd_is_known("future_cmd")   == false);
        CHECK(rpc_cmd_is_known("")             == false);
        CHECK(rpc_cmd_is_known(NULL)           == false);

        /* 大小写敏感 + 必须完整匹配（前缀/后缀/子串/空白都不算命中） */
        CHECK(rpc_cmd_is_known("Query_Status")  == false);
        CHECK(rpc_cmd_is_known("ADD_USER")      == false);
        CHECK(rpc_cmd_is_known("query_status2") == false);
        CHECK(rpc_cmd_is_known("xadd_user")     == false);
        CHECK(rpc_cmd_is_known("add_userx")     == false);
        CHECK(rpc_cmd_is_known(" add_user")     == false);
        CHECK(rpc_cmd_is_known("add_user ")     == false);
    }

    /* ---- 9. rpc_cmd_otp_self_escalating：自带补码流程的指令（当前仅两条） ----
     * ★ 这是三份 OTP 判定里**唯一「漏登记即不安全」**的一份（另两份漏登记都落在
     *   安全侧：needs_otp 漏登记 → 新指令默认要码；is_known 漏登记 → 被当未知拒）。
     *   漏登记本表的后果：该指令被入口统一门控拦掉 ——
     *     · remote_unlock 的 ADR-4 **本地弹码页**被吃掉；
     *     · sync_time 的 |Δ| <= 3600s **常规校时**失效（NTP / RTC 日漂移补偿）。
     *   两者都是**静默失效**：不报错，功能直接没了，功能测试还不一定当场发现。
     *   故把「哪些指令自己承接补码」钉死：多一条、少一条都会红。 */
    {
        CHECK(rpc_cmd_otp_self_escalating("remote_unlock") == true);
        CHECK(rpc_cmd_otp_self_escalating("sync_time")     == true);

        /* 其余 8 条已实现指令一律不豁免 —— 将来谁要新增豁免，必须同时改这里 */
        CHECK(rpc_cmd_otp_self_escalating("query_status")    == false);
        CHECK(rpc_cmd_otp_self_escalating("query_users")     == false);
        CHECK(rpc_cmd_otp_self_escalating("add_user")        == false);
        CHECK(rpc_cmd_otp_self_escalating("del_user")        == false);
        CHECK(rpc_cmd_otp_self_escalating("set_face_enable") == false);
        CHECK(rpc_cmd_otp_self_escalating("set_face_policy") == false);
        CHECK(rpc_cmd_otp_self_escalating("submit_otp")      == false);
        CHECK(rpc_cmd_otp_self_escalating("inject_face")     == false);
        CHECK(rpc_cmd_otp_self_escalating("ota_update")      == false);

        /* NULL / 空 / 大小写 / 前后缀 */
        CHECK(rpc_cmd_otp_self_escalating(NULL)              == false);
        CHECK(rpc_cmd_otp_self_escalating("")                == false);
        CHECK(rpc_cmd_otp_self_escalating("Remote_Unlock")   == false);
        CHECK(rpc_cmd_otp_self_escalating("remote_unlock2")  == false);
        CHECK(rpc_cmd_otp_self_escalating("xremote_unlock")  == false);
    }

    /* ---- 10. 交叉一致性：三份判定互不矛盾 + 入口组合语义 ---- */
    {
        /* 10a. 豁免名单必须是「已实现指令」的子集（与门禁 check 2 同判据）。
         *      豁免一条根本不存在的指令 = 死代码，说明有人改了指令名却漏改豁免。 */
        CHECK(rpc_cmd_is_known("remote_unlock") == true);
        CHECK(rpc_cmd_is_known("sync_time")     == true);

        /* 10b. 豁免名单**必须含**这两条（与门禁 check 3 同判据）。
         *      ★ 为什么子集检查单独不够：把 remote_unlock 从豁免名单整条删掉后，
         *        剩下的 sync_time 仍是子集 → 子集检查照样 PASS，而 ADR-4 弹码页
         *        已经静默失效。所以必须再加一条「存在性」断言。 */
        CHECK(rpc_cmd_otp_self_escalating("remote_unlock") == true);
        CHECK(rpc_cmd_otp_self_escalating("sync_time")     == true);

        /* 10c. 入口统一门控的组合语义：
         *      要码 = 已登记 && needs_otp && !自带补码。
         *      把「当前到底哪几条在入口被要求动态码」钉死（6 条要码 / 4 条免）。
         *      新增指令必须落在这张期望表里，否则本用例会红 —— 逼着人去同步
         *      is_known / needs_otp / self_escalating 三处。 */
        struct { const char * cmd; bool need; } expect[] = {
            { "query_status",    false },  /* needs_otp 豁免：只读查询 */
            { "submit_otp",      false },  /* needs_otp 豁免：鸡生蛋 */
            { "remote_unlock",   false },  /* 自带补码流程（ADR-4 本地弹码页） */
            { "sync_time",       false },  /* 自带补码流程（按时钟跳变幅度判） */
            { "query_users",     true  },
            { "add_user",        true  },
            { "del_user",        true  },
            { "set_face_enable", true  },
            { "set_face_policy", true  },
            { "inject_face",     true  },
        };
        for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
            const char * c = expect[i].cmd;
            /* 前置：参与组合的前提是「已实现」，否则 dispatch 根本走不到门控 */
            CHECK(rpc_cmd_is_known(c) == true);
            const bool gate = rpc_cmd_is_known(c)
                              && rpc_cmd_needs_otp(c)
                              && !rpc_cmd_otp_self_escalating(c);
            CHECK(gate == expect[i].need);
        }
    }

    TEST_RESULT();
}
