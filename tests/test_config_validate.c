/**
 * @file test_config_validate.c
 * 配置校验单测：范围 / 路径 / 组合约束 / 分级 / 钳制。
 *
 * 设计要点：全部直接构造 app_config_t，不读环境变量、不启动 store ——
 * 校验函数是纯函数，测试才能给出确定结论（路径项用 mkdtemp 造临时目录）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/config_validate.h"

static int fails = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while(0)

/* 造一份全部合法的配置；data_dir 由调用方给。 */
static void make_valid(app_config_t * c, const char * dir)
{
    memset(c, 0, sizeof(*c));
    c->face_otp_after        = 3;
    c->face_verify_timeout_s = 10;
    c->virtual_pin_enable    = true;
    c->max_failed            = 5;
    c->lock_seconds          = 30;
    c->log_max_entries       = 500;
    snprintf(c->mqtt_host, sizeof(c->mqtt_host), "%s", "127.0.0.1");
    c->mqtt_port    = 1883;
    c->mqtt_enabled = false;
    c->mqtt_user[0] = '\0';
    c->mqtt_pass[0] = '\0';
    c->mqtt_tls     = false;
    c->mqtt_tls_verify = false;
    c->face_backend = "none";
    c->data_dir     = dir;
}

/* 在报告里找某字段的第一条问题（找不到返回 NULL）。 */
static const cfg_issue_t * find_issue(const cfg_report_t * r, const char * field)
{
    for(int i = 0; i < r->count; i++) {
        if(strcmp(r->items[i].field, field) == 0) return &r->items[i];
    }
    return NULL;
}

int main(void)
{
    char tmpl[] = "/tmp/safe_cfgtest_XXXXXX";
    char * dir = mkdtemp(tmpl);
    CHECK(dir != NULL);

    app_config_t c;
    cfg_report_t r;

    /* 1. 合法配置：零问题 */
    make_valid(&c, dir);
    app_config_validate(&c, &r);
    CHECK(r.total == 0);
    CHECK(r.fatal_count == 0);
    CHECK(r.warn_count == 0);

    /* 2. 数值越界 → WARN（可钳制），不是 FATAL */
    make_valid(&c, dir);
    c.log_max_entries = 0;
    c.max_failed      = 999;
    c.lock_seconds    = 0;
    c.face_otp_after  = 99;
    c.face_verify_timeout_s = 1;
    app_config_validate(&c, &r);
    CHECK(r.warn_count == 5);
    CHECK(r.fatal_count == 0);
    CHECK(find_issue(&r, "log_max_entries") != NULL);
    CHECK(find_issue(&r, "max_failed") != NULL);
    CHECK(find_issue(&r, "lock_seconds") != NULL);
    CHECK(find_issue(&r, "face_otp_after") != NULL);
    CHECK(find_issue(&r, "face_verify_timeout_s") != NULL);

    /* 3. 钳制：全部拉回边界，返回值 = 修改字段数 */
    CHECK(app_config_clamp(&c) == 5);
    CHECK(c.log_max_entries == CFG_LOG_MAX_MIN);
    CHECK(c.max_failed == CFG_MAX_FAILED_MAX);
    CHECK(c.lock_seconds == CFG_LOCK_SECONDS_MIN);
    CHECK(c.face_otp_after == CFG_FACE_OTP_MAX);
    CHECK(c.face_verify_timeout_s == CFG_FACE_TIMEOUT_MIN);
    /* 再钳一次应当零改动（幂等） */
    CHECK(app_config_clamp(&c) == 0);

    /* 4. 数据目录不存在 / 不是目录 → FATAL */
    make_valid(&c, "/tmp/safe_cfgtest_no_such_dir_9163");
    app_config_validate(&c, &r);
    CHECK(r.fatal_count == 1);
    CHECK(find_issue(&r, "data_dir")->severity == CFG_SEV_FATAL);

    make_valid(&c, NULL);
    app_config_validate(&c, &r);
    CHECK(r.fatal_count == 1);              /* 空指针也要被判非法，不能崩 */

    /* 5. 远程通道启用时：host 空 / 端口越界 → FATAL */
    make_valid(&c, dir);
    c.mqtt_enabled = true;
    c.mqtt_host[0] = '\0';
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_host") != NULL);
    CHECK(find_issue(&r, "mqtt_host")->severity == CFG_SEV_FATAL);

    make_valid(&c, dir);
    c.mqtt_enabled = true;
    c.mqtt_port    = 70000;
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_port") != NULL);
    CHECK(find_issue(&r, "mqtt_port")->severity == CFG_SEV_FATAL);

    /* 6. 未启用时端口越界 → 只 WARN（先配错、后来一开就炸，不值得拒绝启动） */
    make_valid(&c, dir);
    c.mqtt_port = 0;
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_port") != NULL);
    CHECK(find_issue(&r, "mqtt_port")->severity == CFG_SEV_WARN);

    /* 7. 只给密码不给用户名 → WARN */
    make_valid(&c, dir);
    snprintf(c.mqtt_pass, sizeof(c.mqtt_pass), "%s", "secret");
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_user") != NULL);
    CHECK(find_issue(&r, "mqtt_user")->severity == CFG_SEV_WARN);

    /* 8. TLS 组合约束 */
    make_valid(&c, dir);
    c.mqtt_enabled = true;
    c.mqtt_tls     = true;
#if defined(SAFE_FEATURE_TLS)
    /* 8a. 要求校验但没有 CA → FATAL */
    c.mqtt_tls_verify = true;
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_ca_file") != NULL);
    CHECK(find_issue(&r, "mqtt_ca_file")->severity == CFG_SEV_FATAL);

    /* 8b. CA 存在但客户端证书缺私钥 → FATAL */
    char ca[512];
    snprintf(ca, sizeof(ca), "%s/ca.crt", dir);
    FILE * f = fopen(ca, "w");
    CHECK(f != NULL);
    if(f) fclose(f);
    snprintf(c.mqtt_ca_file, sizeof(c.mqtt_ca_file), "%s", ca);
    snprintf(c.mqtt_cert_file, sizeof(c.mqtt_cert_file), "%s/device.crt", dir);
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_cert_file") != NULL);
    CHECK(find_issue(&r, "mqtt_cert_file")->severity == CFG_SEV_FATAL);
#else
    /* 8c. 构建未编 TLS 却要求 TLS → FATAL（提前暴露，而不是等连接失败） */
    c.mqtt_tls_verify = true;
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_tls") != NULL);
    CHECK(find_issue(&r, "mqtt_tls")->severity == CFG_SEV_FATAL);
#endif

    /* 9. 没开 TLS 却配了证书 → WARN（多半是「以为开了」） */
    make_valid(&c, dir);
    snprintf(c.mqtt_cert_file, sizeof(c.mqtt_cert_file), "%s", "/etc/safe/device.crt");
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "mqtt_tls") != NULL);
    CHECK(find_issue(&r, "mqtt_tls")->severity == CFG_SEV_WARN);

    /* 10. 人脸后端名非法 → WARN（会静默降级 none，界面像模组坏了） */
    make_valid(&c, dir);
    c.face_backend = "fm255";
    app_config_validate(&c, &r);
    CHECK(find_issue(&r, "face_backend") != NULL);
    CHECK(find_issue(&r, "face_backend")->severity == CFG_SEV_WARN);

    /* 11. 报告容量：问题数超过 CFG_MAX_ISSUES 时 total 仍真实，count 封顶 */
    make_valid(&c, dir);
    c.log_max_entries = 0;
    c.max_failed = 0;
    c.lock_seconds = 0;
    c.face_otp_after = 0;
    c.face_verify_timeout_s = 0;
    c.mqtt_port = 0;
    c.mqtt_pass[0] = 'x';
    c.face_backend = "bogus";
    app_config_validate(&c, &r);
    CHECK(r.total >= 8);
    CHECK(r.count <= CFG_MAX_ISSUES);
    CHECK(r.warn_count + r.fatal_count == r.total);

    /* 12. 严格模式缺省为真（生产安全侧）；显式 0 才放宽 */
    unsetenv("SAFE_CONFIG_STRICT");
    CHECK(app_config_strict() == true);
    setenv("SAFE_CONFIG_STRICT", "0", 1);
    CHECK(app_config_strict() == false);
    setenv("SAFE_CONFIG_STRICT", "1", 1);
    CHECK(app_config_strict() == true);
    unsetenv("SAFE_CONFIG_STRICT");

    /* 13. 入参为 NULL 不崩 */
    app_config_validate(NULL, &r);
    CHECK(r.fatal_count == 1);
    app_config_validate(&c, NULL);          /* 不应崩 */
    CHECK(app_config_clamp(NULL) == 0);

    rmdir(dir);
    if(fails) { printf("[FAIL] %s（%d 项未过）\n", __FILE__, fails); return 1; }
    printf("[PASS] %s\n", __FILE__);
    return 0;
}
