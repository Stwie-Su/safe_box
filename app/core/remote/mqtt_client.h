/**
 * @file mqtt_client.h
 * MQTT 客户端封装（需求 FR-5 / M6）：基于 Eclipse Paho C（MQTTAsync，libpaho-mqtt3a）。
 *
 * 设计要点：
 *   - 独立网络由 Paho 内部线程驱动，本封装只负责「创建 / 连接 / 重连 / 订阅 / 发布」；
 *   - 断线自动重连（指数退避，封顶 8s），上层无感；
 *   - 收到的指令先入线程安全队列，由主线程 rpc_poll() 取走处理（线程安全：MQTT 线程
 *     不得直接操作 LVGL 对象，见需求 3.3）。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 收到 safe/cmd 等消息时的回调（payload 为以 '\0' 结尾的 JSON 字符串） */
typedef void (*mqtt_msg_cb_t)(const char *topic, const char *payload);

/* 启动 MQTT 客户端并后台连接。host 形如 "192.168.150.139"，port=1883。返回 0=OK。 */
int  mqtt_start(const char *host, int port, const char *client_id, mqtt_msg_cb_t cb);

/* 停止并释放（进程退出时调用） */
void mqtt_stop(void);

/* 发布一条消息；qos∈{0,1,2}，retained=是否保留。返回 0=成功。断线时返回 -1。 */
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

/* 主线程取走一条入队消息（rpc_poll 调用）。返回 1 表示取到。 */
int  mqtt_take(char *topic, size_t tcap, char *payload, size_t pcap);

#ifdef __cplusplus
} /*extern "C"*/
#endif
