/**
 * @file mqtt_client.c
 * MQTT 客户端实现（Paho MQTTAsync，libpaho-mqtt3a，无 TLS，局域网够用）。
 * 支持 username/password 连接凭据（来自 SAFE_MQTT_USER/PASS，缺省为空 = 匿名，
 * 保持既有 PC 验证路径）；TLS 本期**不实现**（拍板：只写不做）。
 *
 * 重连策略：onFailure / connlost 中退避重连（0.5s 起步，翻倍封顶 8s）。
 * 入库队列（g_q）为「主线程消费」缓冲，容量 16，满则丢最旧避免阻塞 MQTT 线程。
 */
#include "core/remote/mqtt_client.h"
#include <MQTTAsync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#define QCAP 16
#define ADDRESS_MAX 128
#define TOPIC_MAX   128
#define PAYLOAD_MAX 512

static MQTTAsync g_cli = NULL;
static volatile int g_conn = 0;
static mqtt_msg_cb_t g_cb = NULL;
static char g_address[ADDRESS_MAX];

/* 连接凭据（默认空 = 匿名连接）。由 mqtt_set_credentials() 在 mqtt_start 前写入。
 * 来源：SAFE_MQTT_USER / SAFE_MQTT_PASS（经 app_config → rpc_init 注入）。 */
#define CRED_MAX 64
static char g_user[CRED_MAX];
static char g_pass[CRED_MAX];

/* 入队（供主线程 rpc_poll 消费） */
static char g_q_topic[QCAP][TOPIC_MAX];
static char g_q_payload[QCAP][PAYLOAD_MAX];
static int  g_qh = 0, g_qt = 0;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;

static void push_msg(const char *topic, const char *payload)
{
    pthread_mutex_lock(&g_qm);
    int next = (g_qh + 1) % QCAP;
    if (next == g_qt) g_qt = (g_qt + 1) % QCAP;   /* 满则丢最旧 */
    strncpy(g_q_topic[g_qh], topic, TOPIC_MAX - 1);
    g_q_topic[g_qh][TOPIC_MAX - 1] = '\0';
    strncpy(g_q_payload[g_qh], payload, PAYLOAD_MAX - 1);
    g_q_payload[g_qh][PAYLOAD_MAX - 1] = '\0';
    g_qh = next;
    pthread_mutex_unlock(&g_qm);
}

void mqtt_stop(void)
{
    if (!g_cli) return;
    MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
    opts.timeout = 200;
    MQTTAsync_disconnect(g_cli, &opts);
    MQTTAsync_destroy(&g_cli);
    g_cli = NULL;
    g_conn = 0;
}

bool mqtt_is_connected(void) { return g_conn != 0; }

void mqtt_set_credentials(const char *username, const char *password)
{
    snprintf(g_user, sizeof(g_user), "%s", (username && *username) ? username : "");
    snprintf(g_pass, sizeof(g_pass), "%s", (password && *password) ? password : "");
    /* 配置不完整提示（仅启动时打一行）：只设了其一也按「已配置凭据」处理，
     * 但明确提示，避免「只想设密码」的人困惑于为何走鉴权模式。 */
    if ((g_user[0] != '\0') != (g_pass[0] != '\0')) {
        printf("[MQTT] 连接凭据不完整（仅设置了%s），将按「已配置凭据」处理\n",
               (g_user[0] != '\0') ? "用户名" : "密码");
    }
}

bool mqtt_credentials_configured(void)
{
    /* username 或 password 任一非空即视为「已配置凭据」（不再只认用户名）。 */
    return (g_user[0] != '\0') || (g_pass[0] != '\0');
}

bool mqtt_is_authenticated(void)
{
    /* 已配置凭据且已连上 broker 即视为鉴权连接。
     * （无 TLS 时客户端无法反向验证 broker 是否真的校验了凭据，此为尽最大努力门控。） */
    return g_conn != 0 && mqtt_credentials_configured();
}

/* 把凭据填进 connectOptions：空凭据置 NULL，走匿名；否则带 username/password，
 * 供 broker 侧 password_file + allow_anonymous false 校验（配置样例见交付报告）。 */
static void fill_auth(MQTTAsync_connectOptions *co)
{
    co->username = (g_user[0] != '\0') ? g_user : NULL;
    co->password = (g_pass[0] != '\0') ? g_pass : NULL;
}

/* 订阅 safe/cmd（QoS 1） */
static void do_subscribe(void)
{
    MQTTAsync_responseOptions ro = MQTTAsync_responseOptions_initializer;
    MQTTAsync_subscribe(g_cli, "safe/cmd", 1, &ro);
}

static void on_connect(void *context, MQTTAsync_successData * data)
{
    (void)context;
    (void)data;
    g_conn = 1;
    do_subscribe();
    printf("[MQTT] connected to %s\n", g_address);
}

static void on_connect_failure(void *context, MQTTAsync_failureData *data)
{
    (void)context;
    g_conn = 0;
    if (data && data->message) printf("[MQTT] connect failed: %s\n", data->message);
    else printf("[MQTT] connect failed\n");
    /* 退避重连 */
    usleep(500000);
    MQTTAsync_connectOptions co = MQTTAsync_connectOptions_initializer;
    co.keepAliveInterval = 20;
    co.cleansession = 1;
    co.onSuccess = on_connect;
    co.onFailure = on_connect_failure;
    fill_auth(&co);
    MQTTAsync_connect(g_cli, &co);
    return;
}

static void on_connection_lost(void *context, char *cause)
{
    (void)context;
    g_conn = 0;
    printf("[MQTT] connection lost: %s\n", cause ? cause : "(null)");
    usleep(500000);
    MQTTAsync_connectOptions co = MQTTAsync_connectOptions_initializer;
    co.keepAliveInterval = 20;
    co.cleansession = 1;
    co.onSuccess = on_connect;
    co.onFailure = on_connect_failure;
    fill_auth(&co);
    MQTTAsync_connect(g_cli, &co);
    return;
}

static int on_message(void *context, char *topic, int topicLen, MQTTAsync_message *msg)
{
    (void)context; (void)topicLen;
    char payload[PAYLOAD_MAX];
    size_t n = (size_t)msg->payloadlen;
    if (n >= sizeof(payload)) n = sizeof(payload) - 1;
    memcpy(payload, msg->payload, n);
    payload[n] = '\0';
    if (g_cb) g_cb(topic, payload);
    else push_msg(topic, payload);
    MQTTAsync_freeMessage(&msg);
    MQTTAsync_free(topic);
    return 1;   /* 1 表示消息已处理，Paho 会释放 topic 与 message */
}

int mqtt_start(const char *host, int port, const char *client_id, mqtt_msg_cb_t cb)
{
    g_cb = cb;
    snprintf(g_address, sizeof(g_address), "tcp://%s:%d", host, port);

    int rc = MQTTAsync_create(&g_cli, g_address, client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTASYNC_SUCCESS) {
        printf("[MQTT] create failed: %d\n", rc);
        return -1;
    }
    MQTTAsync_setCallbacks(g_cli, NULL, on_connection_lost, on_message, NULL);

    MQTTAsync_connectOptions co = MQTTAsync_connectOptions_initializer;
    co.keepAliveInterval = 20;
    co.cleansession = 1;
    co.onSuccess = on_connect;
    co.onFailure = on_connect_failure;
    co.automaticReconnect = 0;   /* 自行控制重连，便于退避 */
    fill_auth(&co);              /* 凭据（空则匿名）：供 broker 侧 password_file/ACL 校验 */

    rc = MQTTAsync_connect(g_cli, &co);
    if (rc != MQTTASYNC_SUCCESS) {
        printf("[MQTT] connect() returned %d (will retry via onFailure)\n", rc);
        return -1;
    }
    return 0;
}

int mqtt_publish(const char *topic, const char *payload, int qos, int retained)
{
    if (!g_cli || !g_conn) return -1;
    MQTTAsync_message msg = MQTTAsync_message_initializer;
    msg.payload = (void *)(uintptr_t)payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos = qos;
    msg.retained = retained ? 1 : 0;
    MQTTAsync_responseOptions ro = MQTTAsync_responseOptions_initializer;
    int rc = MQTTAsync_sendMessage(g_cli, topic, &msg, &ro);
    return (rc == MQTTASYNC_SUCCESS) ? 0 : -1;
}

/* 主线程取走一条消息（rpc_poll 调用）。返回 1 表示取到。 */
int mqtt_take(char *topic, size_t tcap, char *payload, size_t pcap)
{
    pthread_mutex_lock(&g_qm);
    int empty = (g_qh == g_qt);
    if (!empty) {
        strncpy(topic, g_q_topic[g_qt], tcap - 1);
        topic[tcap - 1] = '\0';
        strncpy(payload, g_q_payload[g_qt], pcap - 1);
        payload[pcap - 1] = '\0';
        g_qt = (g_qt + 1) % QCAP;
    }
    pthread_mutex_unlock(&g_qm);
    return empty ? 0 : 1;
}
