/**
 * @file mqtt_stub.c
 * MQTT 空实现：构建期没找到 paho-mqtt3a 或显式关闭 SAFE_FEATURE_MQTT 时编译本文件。
 *
 * 目的：远程通道是可选特性，缺库时系统仍要能完整跑起来，
 * 只是不再对外发消息、也不接受指令。调用方代码不需要写 #ifdef。
 */

#include "core/remote/mqtt_client.h"

#include <stdio.h>

int mqtt_start(const char * host, int port, const char * client_id, mqtt_msg_cb_t cb)
{
    (void)host; (void)port; (void)client_id; (void)cb;
    printf("[mqtt] 特性未启用（缺少 paho-mqtt3a 或已关闭 SAFE_FEATURE_MQTT），远程通道停用\n");
    return -1;
}

void mqtt_stop(void)
{
}

int mqtt_publish(const char * topic, const char * payload, int qos, int retained)
{
    (void)topic; (void)payload; (void)qos; (void)retained;
    return -1;
}

bool mqtt_is_connected(void)
{
    return false;
}

void mqtt_set_credentials(const char * username, const char * password)
{
    (void)username; (void)password;
}

bool mqtt_credentials_configured(void)
{
    return false;   /* 无 MQTT 特性 → 不存在鉴权通道 */
}

bool mqtt_is_authenticated(void)
{
    return false;
}

int mqtt_take(char * topic, size_t tcap, char * payload, size_t pcap)
{
    (void)topic; (void)tcap; (void)payload; (void)pcap;
    return 0;
}
