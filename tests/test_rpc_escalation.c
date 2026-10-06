/**
 * @file test_rpc_escalation.c
 * rpc_guard 的「提权门控」子集单测（QA 补缺，2026-10-06）。
 *
 * 锁定两条此前**零覆盖**的安全语义：
 *   1) rpc_cmd_needs_otp  —— 白名单式「接管级操作须管理员动态码」判定。
 *      对应 §10 集成验收「接管级操作仍要求动态码」与 FR-29「升级属接管级操作、
 *      须管理员动态码确认」的可执行判据。
 *   2) rpc_sync_time_gate —— sync_time 的时钟跳变门控（QA-03 第 2 层防御）。
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

    TEST_RESULT();
}
