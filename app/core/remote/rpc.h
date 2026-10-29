/**
 * @file rpc.h
 * 远程指令处理：解析 safe/cmd 里的 JSON，执行对应动作，再把结果发回去。
 *
 * 支持的指令：查状态、查用户、远程开锁、增删用户、开关人脸通道、改人脸策略、
 * 校时、注入人脸、提交动态码。
 * 敏感操作必须带管理员动态码，没带返回 2001，错了返回 2002。
 *
 * 线程：网络线程只负责把消息放进队列，真正的处理在主线程 rpc_poll() 里做
 * （因为要改界面和用户表）。
 *
 * 刻意没做的两件事：远程改 PIN、远程改用户角色。
 * 这两件事的风险在授权模型，不在通道：改管理员 PIN 会让拿着旧密码的现场管理员
 * 直接被锁在外面。要改请到设备上走本地流程。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动远程通道并注册指令处理。开机调一次（配置里关掉时不调）。 */
void rpc_init(const char *host, int port);

/* 主线程每 20ms 调一次：取指令队列并执行。 */
void rpc_poll(void);

/* 每 5 秒上报一次状态。 */
void rpc_publish_status_now(void);

/* 连上 broker 的那一刻发一次「在线」状态（retain），用来盖掉上次掉线留下的「离线」。 */
void rpc_publish_status_retained(void);

/* 上报一条事件（开锁成功 / 拒绝 / 锁定）。由认证状态机的事件回调调。 */
void rpc_publish_event(const char *evt, const char *user, const char *detail, int res);

/* 界面轮询：有没有待处理的「本地弹动态码」请求。有就取出 req_id 返回 true。 */
bool rpc_take_local_otp(char *req_id, size_t cap);

/* 设置事件回调（认证模块用，可以把事件转给别处）。 */
typedef void (*rpc_event_hook_t)(const char *evt, const char *user, const char *detail, int res);
void rpc_set_event_hook(rpc_event_hook_t hook);

#ifdef __cplusplus
} /*extern "C"*/
#endif
