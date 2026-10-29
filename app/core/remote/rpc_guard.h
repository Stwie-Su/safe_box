/**
 * @file rpc_guard.h
 * 远程指令的准入检查：这条指令该不该执行。
 *
 * 检查三件事：
 *   1) 去重 —— 同一条指令（req_id 相同）重复送达时，只执行第一次；
 *   2) 时效 —— 指令带的时间戳和本机差太多，就当过期不当处理；
 *   3) 分级 —— 哪些指令属于敏感操作、哪些需要动态码。
 *
 * 单独成文件是因为这里全是纯逻辑（不连网、不读盘），
 * 可以脱离 MQTT 直接写单元测试。没有全局状态，缓存由调用方传进来。
 *
 * 线程：只在主线程 rpc_poll() 里串行调用，所以不加锁。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 回给对端的错误码。 */
enum rpc_code {
    RPC_CODE_OK           = 0,     /* 成功 */
    RPC_CODE_BAD_PARAM    = 1001,  /* 参数不对 */
    RPC_CODE_BAD_JSON     = 1002,  /* JSON 解析失败 */
    RPC_CODE_CMD_FAIL     = 1003,  /* 未知指令或执行失败 */
    RPC_CODE_DUPLICATE    = 1004,  /* 这个 req_id 已经处理过了 */
    RPC_CODE_STALE_TS     = 1005,  /* 时间戳超出允许范围 */
    RPC_CODE_UNAUTHORIZED = 1006,  /* 未鉴权的连接发了敏感指令 */
    RPC_CODE_DEBUG_DISABLED = 1007,/* 调试指令没有显式开启 */
    RPC_CODE_MISSING_REQ_ID = 1008,/* 敏感指令没带 req_id */
    RPC_CODE_NEED_OTP     = 2001,  /* 需要动态码 */
    RPC_CODE_BAD_OTP      = 2002,  /* 动态码错误或已用过 */
    RPC_CODE_NO_USER      = 2003,  /* 用户不存在 */
    RPC_CODE_LOCKED       = 3001,  /* 系统锁定中 */
};

/* 记住最近多少条 req_id。32 条足够覆盖一次人机交互，线性查找也不慢。 */
#define RPC_REQ_CACHE_CAP 32

/* 允许的时钟偏差（秒）：指令时间戳和本机差超过它就算过期。 */
#define RPC_TS_WINDOW_SEC 120

/* 校时免动态码的跳变上限（秒）：跳 1 小时以内算常规对时，超过就得输动态码。
 * 目的是挡住「一次把钟拨几年」这种攻击，又不影响正常校时。 */
#define RPC_CLOCK_JUMP_FREE_SEC 3600

/* req_id 最大长度（含结尾 '\0'）。 */
#define RPC_REQ_ID_CAP 32

/* 去重表的一个槽位。req_id 为空串表示这个槽位空着。 */
typedef struct {
    char    req_id[RPC_REQ_ID_CAP];
    int64_t ts_seen;   /* 记进来的时间，只用于诊断 */
} rpc_req_slot_t;

/* 定长环形去重表，不动态分配。 */
typedef struct {
    rpc_req_slot_t slots[RPC_REQ_CACHE_CAP];
    int            head;   /* 下一个写哪 */
    int            count;  /* 用了几个 */
} rpc_req_cache_t;

/* 清空（启动时调一次）。 */
void rpc_req_cache_init(rpc_req_cache_t *cache);

/* 判断 req_id 是不是第一次见 —— **必须在产生任何副作用之前调**。
 * true = 第一次，准予执行并记下来；false = 重复，直接拒绝。
 * req_id 为空时不参与去重，恒返回 true（否则所有不带 id 的请求会互相当成重复）。 */
bool rpc_req_cache_admit(rpc_req_cache_t *cache, const char *req_id, int64_t now_sec);

/* 时间戳是否新鲜。ts <= 0 表示对端没给，按兼容处理直接放行；
 * 否则 |now - ts| 超过 window 就算过期。 */
bool rpc_ts_fresh(int64_t ts, int64_t now_sec, int window_sec);

/* 是不是敏感指令（增删用户、开关通道、改策略、远程开锁、注入人脸）。
 * 采用白名单：除了明确只读的（目前只有 query_status），一律当敏感处理。
 * 这样以后新增指令忘了登记，后果是「多要一次鉴权」，而不是「免检执行」。 */
bool rpc_cmd_is_sensitive(const char *cmd);

/* 是不是只读指令（重复执行没影响，可以不做去重）。 */
bool rpc_cmd_is_readonly(const char *cmd);

/* 是不是调试钩子（目前只有 inject_face），这类默认关死。 */
bool rpc_cmd_is_debug_hook(const char *cmd);

/* 是否需要动态码。同样白名单：除了 query_status 和 submit_otp，其余都要。
 * submit_otp 豁免是因为「要求它自己再带一个动态码」讲不通。 */
bool rpc_cmd_needs_otp(const char *cmd);

/* 是不是已经实现的指令（rpc.c 里真有对应分支）。
 * 用来区分「未知指令」和「已知但缺动态码」：未知的必须回 1003，
 * 不能因为缺码就回 2001，那样对端会以为是鉴权问题。
 * ★ 这张表和 rpc.c 的分支是同一事实的两处写法，tools/check_rpc_cmd_table.sh 会比对。 */
bool rpc_cmd_is_known(const char *cmd);

/* 这条指令是不是自己处理动态码（入口的统一检查给它让路）。
 * 目前两条：remote_unlock（没码时要在板子上弹输入页）、sync_time（跳得少就不用码）。
 * ★ 这三张表和 needs_otp 的差异必须有人盯着：这里漏登记，会导致
 *   本地弹码页和自动校时悄悄地失效。 */
bool rpc_cmd_otp_self_escalating(const char *cmd);

/* 校时检查。
 *   target 要设成的时间，now 本机当前时间（**要在改钟之前取**），
 *   has_otp 这次有没有带通过校验的动态码，max_jump 免码的最大跳变秒数。
 * 返回 0 = 放行；RPC_CODE_BAD_PARAM = 时间戳非法；RPC_CODE_NEED_OTP = 跳太多又没带码。
 * 注意：如果攻击者已经成功拨过一次钟，now 就不可信了 ——
 * 这是多一层防护，不是根治；根治要靠 TLS + broker 权限。 */
int rpc_sync_time_gate(int64_t target, int64_t now, bool has_otp, int64_t max_jump);

/* 调试钩子的开关检查：没开启返回 1007，开了但连接没鉴权返回 1006，放行返回 0。 */
int rpc_guard_debug_hook_gate(bool allow, bool authenticated);

/* 解析环境变量开关：只有 "1"/"true"/"yes"/"on" 算开，其余一律算关（安全默认）。 */
bool rpc_env_flag_on(const char *value);

/* 统一准入检查，**必须在产生副作用之前调**。返回 0 = 放行，否则返回要回给对端的错误码：
 *   1008 = 敏感指令缺 req_id；1005 = 时间戳过期；1004 = 重复指令。
 * 规则：去重只管敏感指令（只读的重复查询放行）；时效对所有指令生效（除非没给时间戳）。 */
int rpc_guard_admit(rpc_req_cache_t *cache, const char *cmd, const char *req_id,
                    int64_t ts, int64_t now_sec, int window_sec);

#ifdef __cplusplus
} /*extern "C"*/
#endif
