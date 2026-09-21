/**
 * @file rpc_guard.h
 * 远程指令的「轻量访问控制」原语：req_id 去重、请求时效窗口、命令分级。
 *
 * 为什么独立成文件（而不是塞进 rpc.c）：
 *   rpc.c 依赖 MQTT 栈 + cJSON，任何想覆盖它的单测都得拉起整个 MQTT 栈。
 *   把与网络无关的纯逻辑（定长环形去重表 + 时间窗判定 + 命令分级）下沉到本文件后，
 *   tests/test_rpc.c 可零依赖、确定性地直接驱动这些函数（不需要 broker / 时钟）。
 *   本模块**无全局状态**（缓存由调用方持有），可重入。
 *
 * 线程模型：调用方 rpc_poll() 在 LVGL 主线程串行访问同一份缓存，故本模块不加锁。
 * 若将来出现第二处并发访问，由调用方自行加锁（本模块保持无锁以利测试）。
 *
 * 规约：本文件在 remote/ 下，允许使用 cJSON，但为可移植/可测，实际不引入任何头。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RPC 回执错误码（safe/log 的 code 字段；沿用 rpc.c 既有编号风格）---- */
enum rpc_code {
    RPC_CODE_OK           = 0,     /* 成功 */
    RPC_CODE_BAD_PARAM    = 1001,  /* 参数无效 */
    RPC_CODE_BAD_JSON     = 1002,  /* JSON 解析失败 */
    RPC_CODE_CMD_FAIL     = 1003,  /* 未知指令或执行失败 */
    RPC_CODE_DUPLICATE    = 1004,  /* 【新增】req_id 在去重窗口内已处理过（防重放） */
    RPC_CODE_STALE_TS     = 1005,  /* 【新增】params.ts 超出允许时间窗口 */
    RPC_CODE_UNAUTHORIZED = 1006,  /* 【新增】未鉴权通道发来的敏感指令被拒绝 */
    RPC_CODE_DEBUG_DISABLED = 1007,/* 【新增】调试钩子（inject_face）未显式开启，已禁用 */
    RPC_CODE_MISSING_REQ_ID = 1008,/* 【新增】敏感指令缺少 req_id（去重/防重放需要） */
    RPC_CODE_NEED_OTP     = 2001,  /* 需要动态码 */
    RPC_CODE_BAD_OTP      = 2002,  /* 动态码错误或已使用 */
    RPC_CODE_NO_USER      = 2003,  /* 用户不存在 */
    RPC_CODE_LOCKED       = 3001,  /* 系统锁定中 */
};

/* 去重容量：最近处理的 req_id 条数上限。
 * 取值权衡：太大→内存/查找成本上升；太小→「慢重放」可能绕出窗口。
 * 32 足以覆盖一次人机交互（下发指令 + 其后若干次状态轮询）内出现的任何 req_id，
 * 且线性查找 O(32) 在远程通道（人为下发为主）上完全可忽略。 */
#define RPC_REQ_CACHE_CAP 32

/* 请求时效窗口（秒）：客户端自带 params.ts 时与本机时间比对，超窗即拒。
 * ±120s 兼顾「网络/时钟抖动」与「限制可重放的时间跨度」。
 * 注意：本机时钟不准会误伤合法请求，故 ts 缺省时不校验（向后兼容，见 rpc_ts_fresh）。 */
#define RPC_TS_WINDOW_SEC 120

/* sync_time 的「免动态码跳变上限」（秒）：|target - now| <= 该值 视为常规校时
 * （NTP / RTC 日漂移补偿），只需「鉴权通道 + req_id 去重」；超过则视为
 * **信任根级操作**，必须带有效的管理员动态码（TOTP）才允许改时钟。
 *
 * 3600s（1 小时）的取舍：要覆盖 NTP 常规校时与 RTC 的日漂移，又要在「一次拨几年」
 * 这种攻击面前面立一道门。现场若要求更严可下调（如 300s），代价是正常校时也会被
 * 要求输动态码。 */
#define RPC_CLOCK_JUMP_FREE_SEC 3600

/* req_id 字段最大长度（含结尾 '\0'）。与 rpc.c 中 char req_id[32] 对齐。 */
#define RPC_REQ_ID_CAP 32

/* 去重表槽位。
 * req_id[0] == '\0' 表示该槽位为空/已作废（TTL 到期后被清空，可复用）。 */
/* 去重表槽位 */
typedef struct {
    char    req_id[RPC_REQ_ID_CAP];
    int64_t ts_seen;   /* 记入时的本机 Unix 秒（供诊断；不参与淘汰策略） */
} rpc_req_slot_t;

/* 定长环形去重表（无动态分配） */
typedef struct {
    rpc_req_slot_t slots[RPC_REQ_CACHE_CAP];
    int            head;   /* 下一个写入位置 */
    int            count;  /* 已用槽数（<= RPC_REQ_CACHE_CAP） */
} rpc_req_cache_t;

/* 清空缓存。rpc 启动时调用一次（进程内单实例）。 */
void rpc_req_cache_init(rpc_req_cache_t *cache);

/* req_id 去重判定 —— 必须在**执行任何副作用之前**调用。
 * 返回 true  —— 首次出现：准予处理，并把 req_id 记入缓存；
 * 返回 false —— 判定为重复：窗口内已见过相同 req_id（命中时不修改缓存）。
 * req_id 为 NULL / 空串：不参与去重，恒返回 true 且不记录
 *   （否则所有「无 req_id 的请求」会彼此判重，全部被拒）。 */
bool rpc_req_cache_admit(rpc_req_cache_t *cache, const char *req_id, int64_t now_sec);

/* 请求时效校验。ts <= 0 表示客户端未提供时间戳 → 向后兼容，恒返回 true。
 * ts > 0：|now_sec - ts| > window_sec 视为过期/超前，返回 false。 */
bool rpc_ts_fresh(int64_t ts, int64_t now_sec, int window_sec);

/* 命令分级：是否为「敏感」指令（增删用户 / 通道开关 / 改策略 / 远程开锁 / 注入人脸）。
 * 敏感档 = 有副作用、或能伪造安全结果的那批；其余（含未知指令）返回 false。
 * 敏感档 ⇒ 参与 req_id 去重 + 时效，且（配置凭据时）要求鉴权通道。
 *
 * ★ 改为**白名单式**（默认安全方向）：除明确只读的指令（当前仅 query_status）
 *   外，**一律视为敏感**。原实现是黑名单枚举 —— 于是 submit_otp / sync_time
 *   以及将来新增的任何指令都默认落在「不敏感」档：既不要求鉴权、也不参与去重，
 *   重投会二次执行。改成白名单后，漏登记的后果从「二次执行」变成
 *   「回 1004 让对端重试」，失败方向落在安全的那一侧。 */
bool rpc_cmd_is_sensitive(const char *cmd);

/* 是否为「只读」指令（重放无副作用，故豁免去重）。当前仅 query_status。 */
bool rpc_cmd_is_readonly(const char *cmd);

/* 是否为「能伪造结果的调试钩子」命令（目前仅 inject_face）。
 * 这类命令默认必须关死（门控见 rpc_guard_debug_hook_gate）。 */
bool rpc_cmd_is_debug_hook(const char *cmd);

/**
 * 是否需要管理员动态码（TOTP）。
 *
 * ★ 白名单式（安全默认方向 = 需要）：除**显式豁免**的两条外，一律需要。
 *   豁免名单：
 *     · query_status —— 只读查询，无副作用；
 *     · submit_otp   —— 它就是「提交动态码」这条指令本身，要求它再带一个动态码
 *                       是鸡生蛋问题（会让本地 OTP 会话永远走不通）。
 *   为什么不反过来做成「列出需要的命令」：漏登记的后果会从「新指令意外免检」
 *   变成「新指令被要求输码」—— 后者才是安全的失败方向。
 */
bool rpc_cmd_needs_otp(const char *cmd);

/**
 * sync_time 跳变门控（QA-03 第 2 层防御）。
 *
 *   target   请求要设的目标 Unix 秒（即 params.ts）
 *   now      本机当前 Unix 秒（★ 必须在**校时之前**取，否则门控会被自己绕过）
 *   has_otp  本次请求是否带了（且已通过校验的）管理员动态码
 *   max_jump 允许的「无需 OTP 的最大跳变秒数」（用 RPC_CLOCK_JUMP_FREE_SEC）
 *
 * 返回 0 = 放行；否则返回应回的错误码：
 *   RPC_CODE_BAD_PARAM —— target <= 0（无效时间戳；时钟是 lock_until / valid_until /
 *                         TOTP 的信任根，0 与负数一律不受理）；
 *   RPC_CODE_NEED_OTP  —— 跳变超过 max_jump 且未带动态码。
 *
 * 语义：|target - now| <= max_jump  → 常规校时，靠「鉴权通道 + req_id 去重」即可；
 *       |target - now| >  max_jump  → 信任根级操作，**必须**带有效管理员动态码。
 *
 * ★ 诚实边界（必须写进文档、面试主动说）：
 *   门控依赖 hal_time() 的当前值。若攻击者已成功拨过一次时钟，`now` 就不可信
 *   —— 本门控只能限制「逐步漂移」，挡不住「一次性拨到底后再为所欲为」。
 *   它是**深度防御**，不是根治；根治路径是双向 TLS + broker ACL，让攻击者
 *   根本拿不到「可发指令的通道」。选它是因为 30 行代码就能在根治之前把
 *   「匿名 / 低权限改时钟」这条最粗的口子堵上。
 */
int rpc_sync_time_gate(int64_t target, int64_t now, bool has_otp, int64_t max_jump);

/* 调试钩子门控（务必在副作用之前调用）。
 *   allow         = 是否已显式开启（env SAFE_ALLOW_INJECT=1）；
 *   authenticated = 本端连接是否以鉴权身份建立（已配置凭据且已连上 broker）。
 * 返回 0 = 放行；否则返回应回的错误码：
 *   - 未启用             → RPC_CODE_DEBUG_DISABLED (1007)；
 *   - 已启用但通道未鉴权 → RPC_CODE_UNAUTHORIZED  (1006)。 */
int rpc_guard_debug_hook_gate(bool allow, bool authenticated);

/* 白名单式 env 开关解析（大小写不敏感）：仅 "1"/"true"/"yes"/"on" 视为开启；
 * 其余（NULL、空、"0"、"false"、"no"、"off"、任意乱写）一律**关闭**。
 * 安全默认方向 = 关：任何歧义输入都落到「关闭」，避免 "false"/"no" 被误当开启。 */
bool rpc_env_flag_on(const char *value);

/* 统一准入判定（**务必在副作用之前**调用）。返回 0 = 放行；否则返回应回的错误码：
 *    RPC_CODE_MISSING_REQ_ID (1008) —— 敏感档命令缺 req_id；
 *    RPC_CODE_STALE_TS  (1005) —— 带了 ts 且超窗；
 *    RPC_CODE_DUPLICATE (1004) —— 敏感档命令的 req_id 在去重窗口内重复。
 *
 * 最终语义（定论）：
 *   · 去重 = **仅敏感档**（有副作用）登记并判定；只读档（query_status 等）**豁免**——
 *     重放只读无实际危害，却会让「固定 req_id 轮询状态」白白吃 1004，纯摩擦。
 *   · 时效 = **所有档**（当且仅当 ts > 0）；sync_time 例外——其 params.ts 是
 *     「待设置时间」而非「请求时间戳」，校验会误伤对时/校时指令。
 * 判定顺序：先时效后去重（两条相互独立，顺序仅为可读性）。
 *
 * 为什么把「分级」放进本函数而不是散在 dispatch：语义集中一处 → 单测可完整锁定
 * 「只读重复放行 / 敏感重复拒绝」这一对核心行为（无需拉起 MQTT / broker）。 */
int rpc_guard_admit(rpc_req_cache_t *cache, const char *cmd, const char *req_id,
                    int64_t ts, int64_t now_sec, int window_sec);

#ifdef __cplusplus
} /*extern "C"*/
#endif
