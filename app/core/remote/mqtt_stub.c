/**
 * @file mqtt_stub.c
 * MQTT 空实现：构建期 cJSON 缺失或显式关闭 SAFE_FEATURE_MQTT 时编译本文件。
 *
 * 目的：远程通道是可选特性，缺依赖时系统仍要能完整跑起来，
 * 只是不再对外发消息、也不接受指令。调用方代码不需要写 #ifdef。
 *
 * ★ 铁律：本文件必须与 mqtt_client.c **签名完全一致**（含 R9 T04 新增的 4 个），
 *   否则真实现换 stub 时链接直接失败 —— 本项目踩过「stub 与真实现不一致」的坑。
 */

#include "core/remote/mqtt_client.h"

#include <stdio.h>

int mqtt_start(const char * host, int port, const char * client_id, mqtt_msg_cb_t cb)
{
    (void)host; (void)port; (void)client_id; (void)cb;
    printf("[mqtt] 特性未启用（缺少 cJSON 或已关闭 SAFE_FEATURE_MQTT），远程通道停用\n");
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

/* ---- R9 T06 新增：与 mqtt_client.c 同步实现，保持签名一致 ---- */
void mqtt_set_tls(bool enable, const char *ca_file, const char *cert_file,
                  const char *key_file, bool verify_peer)
{
    (void)enable; (void)ca_file; (void)cert_file; (void)key_file; (void)verify_peer;
}

bool mqtt_is_tls(void)
{
    return false;
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

/* ---- R9 T04 新增：与 mqtt_client.c 同步实现，保持签名一致 ---- */

void mqtt_set_will(const char * topic, const char * payload, int qos, bool retain)
{
    (void)topic; (void)payload; (void)qos; (void)retain;
}

const char * mqtt_state_str(void)
{
    return "idle";
}

void mqtt_stats(uint32_t * connects, uint32_t * reconnects, uint32_t * dropped, uint32_t * queue_full)
{
    if (connects)    *connects    = 0;
    if (reconnects)  *reconnects  = 0;
    if (dropped)     *dropped     = 0;
    if (queue_full)  *queue_full  = 0;
}

size_t mqtt_queued(void)
{
    return 0;
}

/* ---- 可观测性补齐：与 mqtt_client.c 同步实现，保持签名一致 ---- */

/* 签名与 mqtt_client.c 保持一致（新增的三个出口是订阅链路的可观测性，见 #14）。
 * stub 侧一律给 0；granted_qos 给 -1 —— 没有连接就没有"授予的 QoS"，
 * 用 0 会让人误以为曾拿到 QoS0。 */
void mqtt_stats_ex(uint32_t *ping_timeout, uint32_t *retransmit, uint32_t *qos0_fallback,
                   uint32_t *rx_publish, uint32_t *reject_oversize, uint32_t *reject_malformed,
                   uint32_t *suback_reject, uint32_t *suback_timeout, int *granted_qos)
{
    if (ping_timeout)     *ping_timeout     = 0;
    if (retransmit)       *retransmit       = 0;
    if (qos0_fallback)    *qos0_fallback    = 0;
    if (rx_publish)       *rx_publish       = 0;
    if (reject_oversize)  *reject_oversize  = 0;
    if (reject_malformed) *reject_malformed = 0;
    if (suback_reject)    *suback_reject    = 0;
    if (suback_timeout)   *suback_timeout   = 0;
    if (granted_qos)      *granted_qos      = -1;
}

size_t mqtt_tx_queued(void)
{
    return 0;
}

size_t mqtt_inflight_count_now(void)
{
    return 0;
}

uint32_t mqtt_backoff_ms(void)
{
    return 0;
}

uint8_t mqtt_retry_count(void)
{
    return 0;
}
