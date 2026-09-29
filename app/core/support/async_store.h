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
#include "core/auth/unlock_backend.h"

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

/* 网络 psk 读取 / 保存（内部 AES + PBKDF2 派生，量级同 PIN 校验）。 */
void astore_net_get_psk(const char * ssid, astore_str_cb_t cb);
void astore_net_add_wifi(const char * ssid, const char * sec, const char * psk, astore_int_cb_t cb);

/* 安全策略落盘（重写 users.json）。 */
void astore_set_policy(int max_failed, int lock_seconds, astore_int_cb_t cb);

#ifdef __cplusplus
} /*extern "C"*/
#endif
