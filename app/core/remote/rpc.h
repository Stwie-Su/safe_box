/**
 * @file rpc.h
 * RPC 指令框架（需求 FR-5 / M7）：解析 safe/cmd 并分发到业务动作。
 *
 *  - 支持指令见需求 2.6：query_status / query_users / remote_unlock / add_user /
 *    del_user / set_face_enable / set_face_policy / sync_time / inject_face /
 *    submit_otp。
 *  - 敏感操作（远程开锁 / 增删用户 / 切换通道 / 改人脸策略）强制携带管理员 TOTP（FR-3）；
 *    缺 otp 回 2001，错误回 2002。
 *  - 回执统一发 safe/log（带 req_id）；query_status 额外发当前状态快照，
 *    query_users 返回分页的用户清单（id/name/role/enabled/face，绝不含凭据字段）。
 *  - 线程安全：MQTT 线程只负责把消息入队，rpc_poll() 在主线程消费（见 3.3）。
 *
 * ---------------- 已知边界（刻意不做，不是遗漏） ----------------
 * ★ **没有 remote_edit_user / remote_set_pin**：远程只能增删用户、开关人脸通道，
 *   不能改既有用户的 PIN / 角色名。原因：这类操作的复杂度在**安全策略**而不在
 *   通道 —— 改管理员 PIN 会让「持有旧凭据的现场管理员」被远程锁死（设备到手、
 *   网络在别人手里，等于凭据易主）；改角色则是提权/降权操作，需要一套与 TOTP
 *   并行的授权模型（谁能改谁的什么）。在授权模型落地之前把口子开在通道上，
 *   只是让「远程改凭据」这件事看起来可用，实际是把风险从「做不了」变成
 *   「能做但没人审」。要改 PIN / 名字，请到设备上走本地流程（旧 PIN 校验 +
 *   管理员二次确认，见 page_users 的 change_pwd / auth 弹窗）。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 MQTT + 注册指令处理。host/port 指向本地 Broker。 */
void rpc_init(const char *host, int port);

/* 主线程定时调用：取走指令队列并分发。 */
void rpc_poll(void);

/* 周期上报状态（5s 定时器调用）。 */
void rpc_publish_status_now(void);

/* 事件上报：由 auth_fsm 的 event_cb 调用，转发到 safe/log（含 UNLOCK/DENY/LOCKOUT）。 */
void rpc_publish_event(const char *evt, const char *user, const char *detail, int res);

/* UI 轮询：是否有待处理的「本地动态码弹窗」请求（remote_unlock 无 otp）。 */
bool rpc_take_local_otp(char *req_id, size_t cap);

/* 业务层（auth_fsm）事件回调挂钩点 */
typedef void (*rpc_event_hook_t)(const char *evt, const char *user, const char *detail, int res);
void rpc_set_event_hook(rpc_event_hook_t hook);

#ifdef __cplusplus
} /*extern "C"*/
#endif

