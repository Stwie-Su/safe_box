/**
 * @file config.h
 * 系统配置快照：锁定策略、远程通道参数、日志条数、数据目录。
 *
 * 配置从哪来（后面的覆盖前面的）：
 *   环境变量 > users.json 里的策略 > 编译期默认值。
 *
 * 这里只提供「读」，改阈值请走 store 的 user_policy_set_*，
 * 不要在两处各存一份，否则重启后两边会不一致。
 */
#pragma once

#include <stdbool.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CFG_MQTT_HOST_MAX   64
#define APP_CFG_MQTT_USER_MAX   64
#define APP_CFG_MQTT_PASS_MAX   64
#define APP_CFG_MQTT_PATH_MAX   256   /* CA / 证书 / 私钥的文件路径 */
#define APP_CFG_DATA_DIR_MAX    256

/* 日志最多保留多少条，超了滚动删除。
 * 500 是沿用下来的默认值（行为不变），运行期可用环境变量 SAFE_LOG_MAX 覆盖。 */
#define APP_CFG_LOG_MAX_DEFAULT 500

typedef struct {
    /* 认证与锁定 */
    int  face_otp_after;        /* 人脸连续没认出来几次，就转成输动态码 */
    int  face_verify_timeout_s; /* 一次人脸验证最多等多少秒 */
    bool virtual_pin_enable;    /* 虚位密码开关（输入时前后可以多打几位） */

    int  max_failed;            /* 连续失败几次锁定 */
    int  lock_seconds;          /* 锁多久（秒） */

    /* 日志 */
    int  log_max_entries;       /* 最多保留条数，超了滚动 */

    /* 远程通道 */
    char mqtt_host[APP_CFG_MQTT_HOST_MAX];
    int  mqtt_port;
    bool mqtt_enabled;      /* 关掉时整个远程通道不启动 */
    /* 连接 broker 用的用户名 / 密码。都为空 = 匿名连接（PC 调试常用）。
     * 只要填了其中一个，就认为「要求鉴权」，敏感指令只允许鉴权连接执行。 */
    char mqtt_user[APP_CFG_MQTT_USER_MAX];
    char mqtt_pass[APP_CFG_MQTT_PASS_MAX];

    /* MQTT over TLS。默认关闭（明文）。
     * 注意：开了 TLS 但编译时没编进 mbedTLS，连接会直接失败，不会偷偷退回明文——
     * 「以为在加密其实是明文」比「连不上」危险得多。 */
    bool mqtt_tls;               /* true = 走 TLS（端口一般是 8883） */
    bool mqtt_tls_verify;        /* true = 校验服务端证书（需要给 CA） */
    char mqtt_ca_file[APP_CFG_MQTT_PATH_MAX];
    char mqtt_cert_file[APP_CFG_MQTT_PATH_MAX];  /* 双向 TLS 才用；空串 = 不带客户端证书 */
    char mqtt_key_file[APP_CFG_MQTT_PATH_MAX];

    /* 人脸 */
    const char * face_backend;   /* "fm225" / "fake" / "none" */

    /* 数据目录（编译期决定，可用环境变量覆盖） */
    const char * data_dir;
} app_config_t;

/* 生成配置快照。必须在 store_init() 之后调用（要读 users.json 里的策略）。
 * 返回值非 0 表示配置有问题，调用方决定是退出还是继续。 */
safe_err_t app_config_init(void);

/* 取配置（内部静态对象，地址不变，不用释放）。没初始化时返回一套默认值。 */
const app_config_t * app_config(void);

/* 用户改了策略之后调它，把阈值部分同步进快照。 */
void app_config_reload_policy(void);

#ifdef __cplusplus
}
#endif
