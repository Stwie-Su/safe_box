/**
 * @file config.h
 * 系统配置快照：阈值、远程通道参数、日志保留、数据目录。
 *
 * 配置来源优先级：环境变量 > 存储层策略（users.json 的 policy）> 编译期默认值。
 * 本模块只提供只读快照，修改阈值仍走 store 的 user_policy_set_*，
 * 避免同一份配置在两处落地。
 */
#pragma once

#include <stdbool.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CFG_MQTT_HOST_MAX   64
#define APP_CFG_DATA_DIR_MAX    256

/* 日志滚动保留条数的唯一默认值（DESIGN.md §3.3）。
 * 运行期可用环境变量 SAFE_LOG_MAX 覆盖（见 config.c）。 */
#define APP_CFG_LOG_MAX_DEFAULT 500

typedef struct {
    /* 认证与锁定（FR-2 / FR-4 / FR-7） */
    int  face_otp_after;        /* 人脸连续未匹配达此值转动态码 */
    int  face_verify_timeout_s; /* 人脸验证过程超时（秒） */
    bool virtual_pin_enable;    /* 虚位密码开关（FR-18） */
    int  max_failed;            /* 连续失败锁定阈值 */
    int  lock_seconds;          /* 锁定时长 */

    /* 日志（FR 日志保留） */
    int  log_max_entries;   /* 保留条数上限，超出滚动转存 */

    /* 远程通道（FR-5） */
    char mqtt_host[APP_CFG_MQTT_HOST_MAX];
    int  mqtt_port;
    bool mqtt_enabled;      /* 构建期或运行期关闭时为 false */

    /* 人脸（FR-1 通道之一） */
    const char * face_backend;

    /* 数据目录（构建期决定，可用环境变量覆盖） */
    const char * data_dir;
} app_config_t;

/* 初始化：拉通各来源生成快照。需在 store_init() 之后调用。 */
safe_err_t app_config_init(void);

/* 返回快照指针（内部静态，不会变地址）。未初始化时返回带默认值的快照。 */
const app_config_t * app_config(void);

/* 策略变更后重新同步阈值部分。 */
void app_config_reload_policy(void);

#ifdef __cplusplus
}
#endif

