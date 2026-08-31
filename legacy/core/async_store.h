/**
 * @file async_store.h
 * store 的类型化异步包装（DESIGN.md §9.4）。
 *
 * 所有回调都在【主线程】worker_poll() 里执行，回调内可自由操作 LVGL。
 * 传入回调的堆数据（list 等）在回调返回后由框架释放，回调内不得保留其指针。
 * 复合操作（加用户/改密/停用/删除）由各页面用 worker_post 定义整段后台函数，见各页面实现。
 */
#pragma once
#include "core/store.h"
#include "core/unlock_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*astore_users_cb_t)(safe_user_t * list, int count);              /* list 回调后由框架释放 */
typedef void (*astore_logs_cb_t)(log_entry_t * list, int count);               /* list 回调后由框架释放 */
typedef void (*astore_verify_cb_t)(int result, const char * user);             /* result = unlock_result_t */
typedef void (*astore_int_cb_t)(int result);                                   /* 0=成功，或 user_verify_pin 返回值 */
typedef void (*astore_str_cb_t)(int result, const char * str);                 /* net_get_psk 结果 */

/* 读全部用户（用于列表/详情/主页统计） */
void astore_load_users(astore_users_cb_t cb);

/* 日志查询（审计页/主页统计）。evt_filter 为 NULL 不过滤，res_filter < 0 不过滤。 */
void astore_query_log(const char * evt_filter, int res_filter, astore_logs_cb_t cb);

/* 日志追加：fire-and-forget，无需结果。 */
void astore_append_log(const char * evt, const char * user, int res, const char * detail);

/* 开锁验证：后台对每个启用用户做 PBKDF2 校验（含失败计数落盘）。 */
void astore_verify_pin(const char * pin, astore_verify_cb_t cb);

/* 设置门禁：校验 admin PIN（0=通过 1=错误 2=锁定 -1=无此用户）。 */
void astore_verify_admin(const char * pin, astore_int_cb_t cb);

/* 网络 psk 读取 / 保存（内部 AES + PBKDF2 派生，量级同 PIN 校验）。 */
void astore_net_get_psk(const char * ssid, astore_str_cb_t cb);
void astore_net_add_wifi(const char * ssid, const char * sec, const char * psk, astore_int_cb_t cb);

/* 安全策略落盘（重写 users.json）。 */
void astore_set_policy(int max_failed, int lock_seconds, astore_int_cb_t cb);

#ifdef __cplusplus
} /*extern "C"*/
#endif
