/**
 * @file mqtt_client.h
 * 自己写的 MQTT 客户端：连 broker、收发消息、断线自动重连。
 *
 * 为什么不直接用现成库：板子上没有能用的 ARM 版库，交叉编译会退化成空实现，
 * 结果就是「电脑上能连、板子上永远断线」。自己写之后两边跑同一份代码。
 *
 * 结构：
 *   - 一条网络线程负责连接、收发、重连，别的线程不用管；
 *   - 收到的消息先放队列，主线程 rpc_poll() 再取走处理
 *     （网络线程不能碰界面，所以必须绕这一道）；
 *   - 断线自动重连，重试间隔从 0.5 秒开始翻倍，最多 30 秒。
 *
 * 调用顺序（app_main 里）：mqtt_set_credentials / mqtt_set_tls / mqtt_set_will
 *   → mqtt_start() → 之后随时 mqtt_publish()；退出时 mqtt_stop()。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 收到消息时的回调（payload 是以 '\0' 结尾的 JSON 字符串）。
 * 注意：它在**网络线程**执行，不要在里面碰界面。rpc.c 传 NULL，改走队列。 */
typedef void (*mqtt_msg_cb_t)(const char *topic, const char *payload);

/* 启动并后台连接。host 形如 "192.168.150.139"，port 一般是 1883。返回 0 = 成功。 */
int  mqtt_start(const char *host, int port, const char *client_id, mqtt_msg_cb_t cb);

/* 停止并回收网络线程（进程退出时调）。等 1 秒还没退出就放手不管，
 * 不用 pthread_cancel —— 那会让线程停在写了一半的状态。 */
void mqtt_stop(void);

/* 发一条消息。qos 取 0 或 1；retained = broker 是否替我们保存最后一条。
 * 返回 0 = 已放进发送队列（不代表对方收到了）。没启动返回 -1。 */
int  mqtt_publish(const char *topic, const char *payload, int qos, int retained);

/* 现在连上了没有（顶栏指示灯用）。 */
bool mqtt_is_connected(void);

/* 设置连接用的用户名 / 密码，**必须在 mqtt_start 之前**调。
 * 两个都留空 = 匿名连接。只要填了一个，就认为要求鉴权，敏感指令只允许这种连接执行。 */
void mqtt_set_credentials(const char *username, const char *password);

/* 有没有配置连接凭据。 */
bool mqtt_credentials_configured(void);

/* 当前连接是不是带凭据连上的。 */
bool mqtt_is_authenticated(void);

/* 设置 TLS，**必须在 mqtt_start 之前**调。
 * enable=false（默认）走明文；ca_file 为空表示不校验对端证书；
 * cert/key 只在双向 TLS 时需要。
 * ★ 开了 TLS 但编译时没编进 mbedTLS，连接会直接失败，不会偷退回明文。 */
void mqtt_set_tls(bool enable, const char *ca_file, const char *cert_file,
                  const char *key_file, bool verify_peer);

/* 当前连接是否走 TLS。 */
bool mqtt_is_tls(void);

/* 主线程取一条已收到的消息（rpc_poll 调）。返回 1 = 取到了。 */
int  mqtt_take(char *topic, size_t tcap, char *payload, size_t pcap);

/* 设置「遗嘱」：设备意外掉线时 broker 替我们发这条消息。
 * 必须在 mqtt_start 之前调；不调就用默认的「safe/status 里 online=0」。
 * topic 传 NULL 或空串 = 不发遗嘱。 */
void mqtt_set_will(const char *topic, const char *payload, int qos, bool retain);

/* 连接状态的可读名字："idle" / "connecting" / "connected" / "retry"。 */
const char * mqtt_state_str(void);

/* 运行统计：连上几次、重连几次、丢了几条、队列满几次。指针可以传 NULL 表示不关心。 */
void mqtt_stats(uint32_t *connects, uint32_t *reconnects, uint32_t *dropped, uint32_t *queue_full);

/* 下行队列里还积压几条。队列满时新消息不入队、也不回 PUBACK，
 * 让 broker 重发 —— 宁可慢，也不能悄悄丢掉一条指令。 */
size_t mqtt_queued(void);

/* 更细的统计（超时、重传、异常报文等）。任何指针都可传 NULL。
 * granted_qos：-1 = 还没收到过合法的 SUBACK，0/1/2 = broker 实际给的 QoS
 * （broker 有权把 QoS1 降到 0，这里能看出来）。 */
void mqtt_stats_ex(uint32_t *ping_timeout, uint32_t *retransmit, uint32_t *qos0_fallback,
                   uint32_t *rx_publish, uint32_t *reject_oversize, uint32_t *reject_malformed,
                   uint32_t *suback_reject, uint32_t *suback_timeout, int *granted_qos);

/* 上行队列积压条数。 */
size_t mqtt_tx_queued(void);

/* 当前有多少条 QoS1 消息在等确认。 */
size_t mqtt_inflight_count_now(void);

/* 当前重连等待时间（毫秒），没在等待则为 0。 */
uint32_t mqtt_backoff_ms(void);

/* 连续重连了几次。 */
uint8_t mqtt_retry_count(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
