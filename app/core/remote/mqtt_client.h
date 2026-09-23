/**
 * @file mqtt_client.h
 * MQTT 客户端封装（需求 FR-5 / M6）：R9 起改为**自研**实现（单线程 poll +
 * 非阻塞连接 + 自研状态机），不再依赖第三方 MQTT 库。
 *
 * 为什么要自研（R9 的核心收益）：板子上没有可用的 ARM 版第三方 MQTT 库，
 * 交叉编译时会退化成空实现 → **板上远程通道永久断线**。自研后板子与 PC 编同一份
 * 代码，且进程内线程数比原方案净减 1（原方案内部占 2 条线程）。
 *
 * 设计要点：
 *   - 独立网络线程（应用第 4 条自建线程）跑单 poll() 循环，上层无感；
 *   - 断线自动重连（指数退避 + 抖动，封顶 30s），上层无感；
 *   - 收到的指令先入线程安全队列，由主线程 rpc_poll() 取走处理（线程安全：
 *     MQTT 线程不得直接操作 LVGL 对象，见需求 3.3）。
 *
 * ★ 对外契约：既有 8 个函数的签名一个字都不能改（rpc.c / ui.c 依赖），只新增。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 收到 safe/cmd 等消息时的回调（payload 为以 '\0' 结尾的 JSON 字符串）
 * 注意：回调在 **net 线程**执行，只能做线程安全的事。rpc.c 传 NULL（走队列）。 */
typedef void (*mqtt_msg_cb_t)(const char *topic, const char *payload);

/* 启动 MQTT 客户端并后台连接。host 形如 "192.168.150.139"，port=1883。返回 0=OK。 */
int  mqtt_start(const char *host, int port, const char *client_id, mqtt_msg_cb_t cb);

/* 停止并释放（进程退出时调用）。
 * 置停止标志 → 写 self-pipe 唤醒 → join（**带 1000ms 超时**，超时则 detach，
 * 绝不用 pthread_cancel —— 那会让线程停在半写状态、套接字里留半个报文）。
 * 顺带修掉 QA-04（原实现没有停止标志，进程退出时网络线程不会被回收）。 */
void mqtt_stop(void);

/* 发布一条消息；qos∈{0,1}，retained=是否保留。返回 0=已入队。未启动返回 -1。
 * 注意：入队即返回，不等待对端确认；QoS1 的确认与重传由 net 线程负责。 */
int  mqtt_publish(const char *topic, const char *payload, int qos, int retained);

/* 当前是否已连接（供顶栏指示灯 / 状态查询） */
bool mqtt_is_connected(void);

/* 设置 MQTT 连接凭据（username/password）。二者留空 = 匿名连接（默认，保持兼容）。
 * 需在 mqtt_start 之前调用；重复调用以最后一次为准。 */
void mqtt_set_credentials(const char *username, const char *password);

/* 是否已配置连接凭据（username 或 password **任一非空**）。配置后视为「要求鉴权」
 * 模式：仅鉴权连接可执行敏感远程指令（判据落在 rpc.c 的 channel_authorized）。 */
bool mqtt_credentials_configured(void);

/* 当前连接是否以鉴权身份建立（已配置凭据且已连上 broker）。 */
bool mqtt_is_authenticated(void);

/* ---------------- R9 T06 新增：MQTT over TLS ---------------- */

/* 设置 TLS。需在 mqtt_start 之前调用，重复调用以最后一次为准。
 *   enable=false（默认）→ 明文 TCP，保持既有验证路径；
 *   ca_file 为空 = 不校验对端；cert/key 仅双向 TLS 需要。
 * ★ 若 enable=true 而构建未开 SAFE_FEATURE_MQTT_TLS，连接将**失败**而非退回明文。 */
void mqtt_set_tls(bool enable, const char *ca_file, const char *cert_file,
                  const char *key_file, bool verify_peer);

/* 当前连接是否建立在 TLS 之上（已启用 TLS 且已连上）。 */
bool mqtt_is_tls(void);

/* 主线程取走一条入队消息（rpc_poll 调用）。返回 1 表示取到。 */
int  mqtt_take(char *topic, size_t tcap, char *payload, size_t pcap);

/* ---------------- R9 T04 新增（规约 §5.10.1 已预留） ---------------- */

/* 设置遗嘱（LWT）。需在 mqtt_start 之前调用；不调用则用默认
 * 「safe/status + {"online":0,...}，QoS1 + retain」。
 * topic 为 NULL 或空串 = 不发遗嘱。 */
void mqtt_set_will(const char *topic, const char *payload, int qos, bool retain);

/* 当前连接状态的可读字符串："idle" / "connecting" / "connected" / "retry"。 */
const char * mqtt_state_str(void);

/* 运行统计（供诊断与验收；任何指针都可传 NULL 表示不关心）。 */
void mqtt_stats(uint32_t *connects, uint32_t *reconnects, uint32_t *dropped, uint32_t *queue_full);

/* 下行队列积压条数（供背压诊断。满时新到的下行**不入队也不回 PUBACK**，
 * 让 broker 重投 —— 安全设备绝不静默丢指令）。 */
size_t mqtt_queued(void);

/* ---------------- 可观测性补齐（C2 / C4） ---------------- */

/* mqtt_stats() 未覆盖的那几个计数器。任何指针都可传 NULL 表示不关心。
 * 为什么单独开一个而不是改 mqtt_stats() 签名：既有 8 个函数的签名一个字都不能改
 * （rpc.c / ui.c 依赖），只新增 —— 这是本头文件顶部写死的对外契约。 */
void mqtt_stats_ex(uint32_t *ping_timeout, uint32_t *retransmit, uint32_t *qos0_fallback,
                   uint32_t *rx_publish, uint32_t *reject_oversize, uint32_t *reject_malformed);

/* 上行队列积压条数（与 mqtt_queued() 的下行成对，用于区分哪个方向在丢）。 */
size_t mqtt_tx_queued(void);

/* 当前 inflight（QoS1 未确认报文）条数。 */
size_t mqtt_inflight_count_now(void);

/* 当前退避时长（ms）；不在退避中则为 0。
 * ★ 退避曲线的最小可用暴露 —— 有了它才谈得上"实测退避序列"。 */
uint32_t mqtt_backoff_ms(void);

/* 当前连续重连次数（状态机 retry_count 的镜像）。 */
uint8_t mqtt_retry_count(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
