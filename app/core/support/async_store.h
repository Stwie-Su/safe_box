/**
 * @file async_store.h
 * store 的类型化异步包装（DESIGN.md §9.4）。
 *
 * 所有回调都在【主线程】worker_poll() 里执行，回调内可自由操作 LVGL。
 * 传入回调的堆数据（list 等）在回调返回后由框架释放，回调内不得保留其指针。
 * 复合操作（加用户/改密/停用/删除）由各页面用 worker_post 定义整段后台函数，见各页面实现。
 */
#pragma once
#include "core/store/store.h"
#include "core/auth/auth_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*astore_users_cb_t)(safe_user_t * list, int count);              /* list 回调后由框架释放 */
typedef void (*astore_logs_cb_t)(log_entry_t * list, int count);               /* list 回调后由框架释放 */
typedef void (*astore_int_cb_t)(int result);                                   /* 0=成功，或 user_verify_pin 返回值 */
typedef void (*astore_str_cb_t)(int result, const char * str);                 /* net_get_psk 结果 */

/* 读全部用户（用于列表/详情/主页统计） */
void astore_load_users(astore_users_cb_t cb);

/* S1：最近一次加载是否**失败**（而非「真的没有数据」）。
 * 加载失败时回调收到的同样是 (NULL, 0)，UI 必须靠它来区分，
 * 否则会把「读取失败」显示成空态、并在数据损坏时诱导用户去覆盖。 */
bool astore_users_load_failed(void);
bool astore_logs_load_failed(void);

/* 日志查询（审计页/主页统计）。evt_filter 为 NULL 不过滤，res_filter < 0 不过滤。 */
void astore_query_log(const char * evt_filter, int res_filter, astore_logs_cb_t cb);

/* 日志追加：fire-and-forget，无需结果。 */
void astore_append_log(const char * evt, const char * user, int res, const char * detail);

/* 用户 / 凭据变更上报：广播 EV_USER_CHANGED（载荷见 event_bus.h 的 ev_user_changed_t）。
 *
 * ★ 为什么这个「上报」入口在 async_store 里，而不是 store.c 或各 UI 页：
 *   1) store.c 是纯数据层（只负责 users.json / safe.log 落盘），让它依赖事件总线
 *      等于把「写盘」和「发布订阅」绑死 —— 数据层不该知道有订阅者这回事。
 *   2) 所有变更点都跑在 **worker 线程**（store 写操作由 worker 单线程持有，QA-20），
 *      而总线的订阅回调会操作 LVGL，**只能主线程调 event_bus_publish**。
 *      这里用 event_bus_post 入队、等主线程 event_bus_pump 再派发，跨线程是安全的。
 *      async_store 本来就是「worker → 主线程」的桥，放这里不必新造一条通道。
 *   3) 各页面只管调这一个函数，不必各自拼 POD、各自判断自己在哪个线程。
 *
 * 与 astore_append_log 是**两件事**：前者只写本地 safe.log，后者只上送远程通道。
 * 调用点应成对出现（本地留痕 + 远程可观测），但失败互不影响 ——
 * 上报失败（MQTT 断线 / 队列满）绝不阻塞本地变更，这是安全设备的硬要求。 */
void astore_report_user_event(const char * evt, const char * user, int res, const char * detail);

/* 设置门禁：校验 admin PIN（0=通过 1=错误 2=锁定 -1=无此用户）。 */
void astore_verify_admin(const char * pin, astore_int_cb_t cb);

/* 管理员**动态码**二次确认（FR-3 敏感操作，如改管理员密码）。
 * 与 astore_verify_admin 只是一张凭据的区别（PIN vs TOTP），同样必须在 worker 执行：
 * auth_admin_verify_totp 内部要 user_load_all（全表读盘）+ user_update
 * （回写 last_otp_counter 防重放计数器、清零失败计数）——整表读写不能在主线程。
 * 回调 result：0=通过 1=错误 2=该码已使用（防重放） */
void astore_admin_verify_totp(const char * code, astore_int_cb_t cb);

/* ---------------- 修改用户（FR-9 v1.11「修改入口」） ----------------
 *
 * 背景：用户类别（角色）与临时授权的两项限制原先只有「创建入口」——一旦设错只能
 * 删号重建，而删号会连带删除人脸凭据。故把改名 / 改 PIN / 改角色 / 改限制收敛成
 * **同一个作业、一次落盘**。
 *
 * ★ 为什么必须合并成一次提交（对应历史缺陷 B3）：原实现是「先改名落盘并记审计，
 *   再验旧 PIN」，旧 PIN 错时名字已经改了、审计也记了，UI 却报「旧 PIN 错误」——
 *   部分成功的副作用撤不回来。现在所有校验在**落盘之前**做完，任何一条不通过
 *   就整单作废：**要么全改，要么一个字段都不改**。
 *
 * ★ 为什么在 worker 里做（而不是主线程读快照 → 算好 → worker 写）：与
 *   astore_auth_side 同理，「读→改→写」必须由同一个线程一口气做完，否则丢失更新。
 *
 * 结果码见 modify_result_t（UI 直接映射成文案，不要把负数当成功）。 */
typedef enum {
    MODIFY_OK            =  0,   /* 成功 */
    MODIFY_ERR_OLDPIN    =  1,   /* 旧 PIN 错误（沿用 user_verify_pin 的语义） */
    MODIFY_ERR_DUPNAME   =  2,   /* 新用户名已存在 */
    MODIFY_ERR_LAST_ADMIN=  3,   /* 至少保留一名启用中的管理员 */
    MODIFY_ERR_ROLE      =  4,   /* 角色非法 */
    MODIFY_ERR_LIMIT     =  5,   /* 次数上限须大于已用次数（否则等于改配置即删号） */
    MODIFY_ERR_STORE     = -1,   /* 无此用户 / 读盘或写盘失败 */
} modify_result_t;

typedef struct {
    int     id;
    char    new_name[32];   /* 空串 = 不改（原样保留） */
    char    old_pin[16];    /* do_pin 时必填：改 PIN 必须先证明知道旧 PIN */
    char    new_pin[16];    /* 空串 = 不改 PIN */
    char    role[16];       /* 空串 = 不改角色；合法值 admin / user / temp */
    int64_t valid_until;    /* 新角色为 temp 时生效（0 = 不限）；非 temp 一律清零 */
    int     use_limit;      /* 同上 */
    int     result;         /* 输出：modify_result_t */
} modify_user_req_t;

/* 提交一个「修改用户」作业。req 内的字符串会被拷进作业，调用后可立即复用/释放。
 * cb 在主线程 worker_poll 派发，收到的是 req->result 同值的 result。 */
void astore_modify_user(const modify_user_req_t * req, astore_int_cb_t cb);

/* 网络 psk 读取 / 保存（内部 AES + PBKDF2 派生，量级同 PIN 校验）。 */
void astore_net_get_psk(const char * ssid, astore_str_cb_t cb);
void astore_net_add_wifi(const char * ssid, const char * sec, const char * psk, astore_int_cb_t cb);

/* 安全策略落盘（重写 users.json）。 */
void astore_set_policy(int max_failed, int lock_seconds, astore_int_cb_t cb);

/* ---------------- 认证路径的用户表副作用（2026-10-07） ----------------
 *
 * 背景：技术路线规约 §3.1 的并发纪律「认证路径零同步落盘」要求 auth_fsm 的回调与
 * 判定路径内不得直接调 user_update / log_append。但认证落下的用户表改动**不只是
 * 写**——它们是「读→改→写」三步：
 *   - 失败：failed_attempts++，达 max_failed 置 lock_until（ADR-3 用户级锁定）；
 *   - 开锁：清零 failed_attempts / lock_until；临时用户 used_count++ 并在用尽时删除；
 *   - FR-21：人脸凭据不一致 → 本地 face_enable 置 false；
 *   - FR-9：临时授权过期 / 用尽 → 级联删除用户（含模组模板号回填）。
 *
 * ★ 为什么必须整段下沉、而不是「主线程读快照 → 算好新值 → worker 写」：
 *   那样做会**丢失更新**。人脸连续出帧时 bump_fail_streak 可能在一帧内被调多次，
 *   每次都基于同一份快照算出 failed_attempts = 旧值+1，最终落盘只 +1 —— 失败计数
 *   形同虚设，max_failed 永远打不满。读改写必须由**同一个线程**一口气做完。
 *
 * ★ 顺序保证从哪来：worker 是单线程 FIFO 队列。前一个「失败计数」作业写盘完成后，
 *   后一个 PIN 校验作业（auth_verify_pin 走 user_load_all 读**真实文件**）才执行，
 *   因此「已锁定」的判定不会被异步化撕开窗口。这是本设计成立的前提。
 *
 * 四类副作用共用一个作业类型 + op 分派（而不是四个独立作业）：它们的输入都是
 * 「一个用户名」，都是在 worker 内做同一形态的读改写，分开写只会复制四遍样板。 */
typedef enum {
    AUTH_SIDE_FAIL_BUMP = 1,     /* 用户级失败计数 +1，达 max_failed 置 lock_until */
    AUTH_SIDE_UNLOCK_TOUCH,      /* 开锁成功后：清零计数；临时用户计数 / 用尽则删除 */
    AUTH_SIDE_FACE_INVALIDATE,   /* FR-21：本地人脸凭据标记失效（face_enable=false） */
    AUTH_SIDE_TEMP_REVOKE,       /* FR-9：临时授权失效，级联删除用户 */
} auth_side_op_t;

/**
 * 认证副作用完成回调（**主线程** worker_poll 派发）。
 *   result     0 = 成功；-1 = 用户不存在或写盘失败（调用方只作提示，不改安全结论）
 *   face_id_rm >= 0 表示本次级联删除带出了一个模组模板号，调用方应在主线程
 *                   调 hal_face_delete_async(face_id_rm)；< 0 表示无需删
 *   removed    true = 该用户已被删除（临时授权用尽 / 失效）
 */
typedef void (*astore_auth_side_cb_t)(int result, int32_t face_id_rm, bool removed);

/**
 * 提交一个认证副作用作业。cb 可为 NULL（fire-and-forget，无需回执）。
 * user 为 NULL 或空串时直接回调 result=-1（无归属对象的副作用无从谈起）。
 *
 * revoke_detail：仅当本次作业**真的删除了用户**时（UNLOCK_TOUCH 用尽、
 * TEMP_REVOKE）由 worker 落一条 ALARM 审计；其余 op 传 NULL。
 * ★ 为什么这条日志在 worker 里写而不是主线程 done 里写：主线程此刻只知道
 *   快照里的 used_count，「到底删没删」只有 worker 知道——让判定的那一方
 *   同时负责记录，才不会出现「没删成却记了审计」的假账。
 */
void astore_auth_side(auth_side_op_t op, const char *user, const char *revoke_detail,
                      astore_auth_side_cb_t cb);

#ifdef __cplusplus
} /*extern "C"*/
#endif
