/**
 * @file config.c
 * 系统配置快照实现。
 *
 * 阈值类配置的唯一数据源是存储层（users.json 的 policy），这里只做只读镜像，
 * 避免"改了阈值却有两份值"的问题。网络与数据目录来自环境变量或编译期常量。
 */

#include "core/config.h"
#include "core/config_validate.h"
#include "core/store/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 编译期默认值：mqtt 主机与数据目录来自 app_version.h */
#include "app_version.h"

#define DEFAULT_MQTT_HOST   "127.0.0.1"
#define DEFAULT_MQTT_PORT   1883

static app_config_t s_cfg = {
    .face_otp_after        = 3,
    .face_verify_timeout_s = 10,
    .virtual_pin_enable    = true,
    .max_failed            = 5,
    .lock_seconds          = 30,
    .log_max_entries = APP_CFG_LOG_MAX_DEFAULT,
    .mqtt_host       = DEFAULT_MQTT_HOST,
    .mqtt_port       = DEFAULT_MQTT_PORT,
    .mqtt_enabled    = false,
    .mqtt_user       = "",
    .mqtt_pass       = "",
    .face_backend    = SAFE_FACE_BACKEND,
    .data_dir        = SAFE_DATA_DIR,
};

static int env_int(const char * name, int fallback)
{
    const char * v = getenv(name);
    if(v == NULL || *v == '\0') return fallback;
    return atoi(v);
}

static void env_str(const char * name, char * out, size_t cap, const char * fallback)
{
    const char * v = getenv(name);
    snprintf(out, cap, "%s", (v && *v) ? v : fallback);
}

void app_config_reload_policy(void)
{
    const safe_policy_t * p = user_policy();
    if(p == NULL) return;
    s_cfg.face_otp_after        = p->face_otp_after;
    s_cfg.face_verify_timeout_s = p->face_verify_timeout_s;
    s_cfg.virtual_pin_enable    = p->virtual_pin_enable;
    s_cfg.max_failed            = p->max_failed;
    s_cfg.lock_seconds          = p->lock_seconds;
}

safe_err_t app_config_init(void)
{
    app_config_reload_policy();

    s_cfg.log_max_entries = env_int("SAFE_LOG_MAX", s_cfg.log_max_entries);
    s_cfg.mqtt_port       = env_int("SAFE_MQTT_PORT", s_cfg.mqtt_port);
    env_str("SAFE_MQTT_HOST", s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host), DEFAULT_MQTT_HOST);
    /* 连接凭据（FR-24）：缺省为空 = 匿名连接，保持既有 PC 验证路径不变。
     * 配置后 rpc.c 只放行「鉴权连接」执行敏感指令（见 rpc.c::channel_authorized）。 */
    env_str("SAFE_MQTT_USER", s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user), "");
    env_str("SAFE_MQTT_PASS", s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass), "");

    /* R9 T06：TLS。SAFE_MQTT_TLS=1 启用；证书路径缺省为空。
     * ★ 校验开关默认跟 TLS 一起开（tls=1 且未显式关 → verify=1）：自签 CA 场景下
     *   「开了 TLS 却不校验」等于没防中间人，故默认取安全侧；仅在连通性测试期
     *   显式设 SAFE_MQTT_TLS_VERIFY=0 放行。 */
    {
        const char * t = getenv("SAFE_MQTT_TLS");
        s_cfg.mqtt_tls = (t != NULL && *t != '\0' && strcmp(t, "0") != 0);
    }
    env_str("SAFE_MQTT_CAFILE", s_cfg.mqtt_ca_file,   sizeof(s_cfg.mqtt_ca_file),   "");
    env_str("SAFE_MQTT_CERT",   s_cfg.mqtt_cert_file, sizeof(s_cfg.mqtt_cert_file), "");
    env_str("SAFE_MQTT_KEY",    s_cfg.mqtt_key_file,  sizeof(s_cfg.mqtt_key_file),  "");
    {
        const char * v = getenv("SAFE_MQTT_TLS_VERIFY");
        s_cfg.mqtt_tls_verify = (v == NULL || *v == '\0') ? s_cfg.mqtt_tls
                                                          : (strcmp(v, "0") != 0);
    }

    const char * off = getenv("SAFE_MQTT_OFF");
    s_cfg.mqtt_enabled = (off == NULL || *off == '\0' || strcmp(off, "0") == 0);

    const char * dd = getenv("SAFE_DATA_DIR");
    s_cfg.data_dir = (dd && *dd) ? dd : SAFE_DATA_DIR;

    /* 人脸后端名：环境变量 > 编译期默认（-DSAFE_FACE_BACKEND=fake/fm225/none）。
     * D2：横幅与 face_service 都取本字段，杜绝"横幅显示 fake、实际跑 fm225"。 */
    const char * fb = getenv("SAFE_FACE_BACKEND");
    s_cfg.face_backend = (fb && *fb) ? fb : SAFE_FACE_BACKEND;

    /* 配置校验（生产级补齐）：范围/路径/组合约束在这里一次性查完。
     *
     * 为什么不在各字段赋值处顺手 if：那样每条规则都散在赋值旁边，
     * 新增一个配置项就要记得再加一段，且无法整体报告；集中成一次遍历后，
     * 规则表就是唯一事实来源，单元测试也能直接喂结构体断言。
     *
     * 分级：
     *   WARN  → 钳制到合法范围后继续（阈值写错不该让设备起不来）；
     *   FATAL → 严格模式拒绝启动；非严格模式关闭远程通道后继续
     *           （绝不把「要求 TLS 但没 CA」降级成明文连接）。
     */
    {
        cfg_report_t rep;
        app_config_validate(&s_cfg, &rep);
        app_config_report(&rep);

        if(rep.fatal_count > 0) {
            /* fatal_blocking = 不可降级的 FATAL（数据目录不可写这类）：
             * 即使非严格模式也必须拒绝启动 —— 关掉远程通道救不了它。 */
            if(app_config_strict() || rep.fatal_blocking > 0) {
                fprintf(stderr,
                        "[CONFIG] 拒绝启动：%d 项 FATAL（其中不可降级 %d）%s\n",
                        rep.fatal_count, rep.fatal_blocking,
                        app_config_strict() ? "" : "，非严格模式也无法降级");
                if(rep.fatal_blocking == 0) {
                    fprintf(stderr,
                            "[CONFIG] 确认配置无误后可设 SAFE_CONFIG_STRICT=0 临时放行\n");
                }
                return SAFE_ERR_PARAM;
            }
            fprintf(stderr,
                    "[CONFIG] 非严格模式：关闭远程通道后继续（不会退化成明文）\n");
            s_cfg.mqtt_enabled = false;
        }
        app_config_clamp(&s_cfg);
    }

    return SAFE_OK;
}

const app_config_t * app_config(void)
{
    return &s_cfg;
}

