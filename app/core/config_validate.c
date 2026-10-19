/**
 * @file config_validate.c
 * 配置校验实现：范围 → 字符串 → 路径 → 组合约束。
 *
 * 纪律：本文件不读环境变量、不分配堆内存、不修改入参。这样同一份输入在任何
 * 环境下都给同一结论（路径存在性除外），单元测试才能断言确定值。
 */
#include "core/config_validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------
 * 函数：add_issue（文件内私有）
 * 功能：追加一条问题；超过 CFG_MAX_ISSUES 只累加计数，保证 total 真实。
 * 参数：degradable —— 仅对 FATAL 有意义：true = 可降级（关远程通道），
 *       false = 必须拒绝启动（数据目录这类没了就没法运行）。
 * ------------------------------------------------------------------ */
static void add_issue(cfg_report_t * r, cfg_severity_t sev, const char * field,
                      bool has_value, bool degradable, long value, const char * rule)
{
    if(r == NULL) return;
    r->total++;
    if(sev == CFG_SEV_FATAL) {
        r->fatal_count++;
        if(!degradable) r->fatal_blocking++;
    }
    else {
        r->warn_count++;
    }

    if(r->count >= CFG_MAX_ISSUES) return;
    cfg_issue_t * it = &r->items[r->count++];
    it->severity   = sev;
    it->field      = field;
    it->has_value  = has_value;
    it->degradable = degradable;
    it->value      = value;
    it->rule       = rule;
}

/* 数值范围：越界记 WARN（可钳制）。 */
static void check_range(cfg_report_t * r, const char * field, long v, long lo, long hi,
                        const char * rule)
{
    if(v < lo || v > hi) add_issue(r, CFG_SEV_WARN, field, true, true, v, rule);
}

/* 字符串非空 + 不超容量。 */
static void check_nonempty(cfg_report_t * r, const char * field, const char * s,
                           size_t cap, cfg_severity_t sev, bool degradable,
                           const char * rule)
{
    if(s == NULL || *s == '\0') {
        add_issue(r, sev, field, false, degradable, 0, rule);
        return;
    }
    if(strlen(s) >= cap) add_issue(r, sev, field, false, degradable, 0, rule);
}

/* 路径：存在 + 可读；want_dir=true 时还要求是目录；want_write=true 时要求可写。 */
static void check_path(cfg_report_t * r, const char * field, const char * path,
                       bool want_dir, bool want_write, cfg_severity_t sev,
                       bool degradable, const char * rule)
{
    if(path == NULL || *path == '\0') {
        add_issue(r, sev, field, false, degradable, 0, rule);
        return;
    }

    struct stat st;
    if(stat(path, &st) != 0) {
        add_issue(r, sev, field, false, degradable, 0, rule);
        return;
    }
    if(want_dir && !S_ISDIR(st.st_mode)) {
        add_issue(r, sev, field, false, degradable, 0, rule);
        return;
    }
    if(want_write && access(path, W_OK) != 0) {
        add_issue(r, sev, field, false, degradable, 0, rule);
    }
}

void app_config_validate(const app_config_t * cfg, cfg_report_t * out)
{
    if(out == NULL) return;
    memset(out, 0, sizeof(*out));
    if(cfg == NULL) {
        add_issue(out, CFG_SEV_FATAL, "config", false, false, 0, "配置快照为空");
        return;
    }

    /* ---- 1. 数值范围（store setter 同款边界，避免两处写两个数） ---- */
    check_range(out, "log_max_entries", cfg->log_max_entries,
                CFG_LOG_MAX_MIN, CFG_LOG_MAX_MAX, "1..100000");
    check_range(out, "face_otp_after", cfg->face_otp_after,
                CFG_FACE_OTP_MIN, CFG_FACE_OTP_MAX, "1..10");
    check_range(out, "face_verify_timeout_s", cfg->face_verify_timeout_s,
                CFG_FACE_TIMEOUT_MIN, CFG_FACE_TIMEOUT_MAX, "3..120");
    check_range(out, "max_failed", cfg->max_failed,
                CFG_MAX_FAILED_MIN, CFG_MAX_FAILED_MAX, "1..10");
    check_range(out, "lock_seconds", cfg->lock_seconds,
                CFG_LOCK_SECONDS_MIN, CFG_LOCK_SECONDS_MAX, "5..3600");

    /* ---- 2. 数据目录（不合法就没法存用户表与日志 → 不可降级） ---- */
    check_nonempty(out, "data_dir", cfg->data_dir, APP_CFG_DATA_DIR_MAX,
                   CFG_SEV_FATAL, false, "非空且长度 < 256");
    if(cfg->data_dir != NULL && *cfg->data_dir != '\0') {
        check_path(out, "data_dir", cfg->data_dir, true, true,
                   CFG_SEV_FATAL, false, "目录存在且可写");
    }

    /* ---- 3. 远程通道（坏了可以关掉远程继续本地开锁 → 可降级） ---- */
    if(cfg->mqtt_enabled) {
        check_nonempty(out, "mqtt_host", cfg->mqtt_host, APP_CFG_MQTT_HOST_MAX,
                       CFG_SEV_FATAL, true, "启用远程通道时必须非空且长度 < 64");
        /* 端口越界在启用时是 FATAL：连接必然失败，且失败原因会被误判成网络问题。 */
        if(cfg->mqtt_port < 1 || cfg->mqtt_port > 65535) {
            add_issue(out, CFG_SEV_FATAL, "mqtt_port", true, true,
                      cfg->mqtt_port, "1..65535");
        }
    }
    else {
        /* 未启用时端口仍应合法，避免「先配错、后来一开就炸」。 */
        if(cfg->mqtt_port < 1 || cfg->mqtt_port > 65535) {
            add_issue(out, CFG_SEV_WARN, "mqtt_port", true, true,
                      cfg->mqtt_port, "1..65535");
        }
    }

    /* 只给密码不给用户名 = 配错了（鉴权判定要求二者均非空）。 */
    if(cfg->mqtt_pass[0] != '\0' && cfg->mqtt_user[0] == '\0') {
        add_issue(out, CFG_SEV_WARN, "mqtt_user", false, true, 0,
                  "给了 mqtt_pass 却没有 mqtt_user");
    }

    /* ---- 4. TLS 组合约束（失败闭合：宁可连不上，不可「以为加密其实明文」） ---- */
    if(cfg->mqtt_tls) {
#if !defined(SAFE_FEATURE_TLS)
        /* 构建时没编 TLS，运行期却要求 TLS：连接会失败（mqtt_client 的失败闭合）。
         * 这里提前报出来，避免运维在「远程连不上」上绕圈。可降级 = 关远程通道。 */
        add_issue(out, CFG_SEV_FATAL, "mqtt_tls", false, true, 0,
                  "要求 TLS，但本构建未启用 TLS（需 -DSAFE_FEATURE_MQTT_TLS=ON）");
#else
        if(cfg->mqtt_tls_verify) {
            check_path(out, "mqtt_ca_file", cfg->mqtt_ca_file, false, false,
                       CFG_SEV_FATAL, true,
                       "校验服务端证书时必须提供可读的 CA 文件");
        }
        bool has_cert = cfg->mqtt_cert_file[0] != '\0';
        bool has_key  = cfg->mqtt_key_file[0]  != '\0';
        if(has_cert != has_key) {
            add_issue(out, CFG_SEV_FATAL, "mqtt_cert_file", false, true, 0,
                      "客户端证书与私钥必须成对提供");
        }
        else if(has_cert) {
            check_path(out, "mqtt_cert_file", cfg->mqtt_cert_file, false, false,
                       CFG_SEV_FATAL, true, "客户端证书必须可读");
            check_path(out, "mqtt_key_file", cfg->mqtt_key_file, false, false,
                       CFG_SEV_FATAL, true, "客户端私钥必须可读");
        }
#endif
    }
    else {
        /* 没开 TLS 却配了一堆证书：多半是「以为开了」。告警但不阻断。 */
        if(cfg->mqtt_ca_file[0] != '\0' || cfg->mqtt_cert_file[0] != '\0' ||
           cfg->mqtt_key_file[0] != '\0') {
            add_issue(out, CFG_SEV_WARN, "mqtt_tls", false, true, 0,
                      "配置了证书但 mqtt_tls=false，证书不会生效");
        }
    }

    /* ---- 5. 人脸后端名（写错会静默降级成 none，界面像「模组坏了」） ---- */
    if(cfg->face_backend != NULL && *cfg->face_backend != '\0') {
        const char * ok[] = { "fake", "fm225", "none" };
        bool found = false;
        for(size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            if(strcmp(cfg->face_backend, ok[i]) == 0) { found = true; break; }
        }
        if(!found) {
            add_issue(out, CFG_SEV_WARN, "face_backend", false, true, 0,
                      "合法值：fake / fm225 / none");
        }
    }
}

void app_config_report(const cfg_report_t * r)
{
    if(r == NULL) return;

    if(r->total == 0) {
        fprintf(stderr, "[CONFIG] 校验通过（0 项问题）\n");
        return;
    }

    fprintf(stderr, "[CONFIG] 校验发现 %d 项问题（FATAL %d [其中不可降级 %d] / WARN %d）\n",
            r->total, r->fatal_count, r->fatal_blocking, r->warn_count);

    for(int i = 0; i < r->count; i++) {
        const cfg_issue_t * it = &r->items[i];
        const char * sev = (it->severity == CFG_SEV_FATAL) ? "FATAL" : "WARN ";
        const char * deg = (it->severity == CFG_SEV_FATAL && !it->degradable)
                               ? "（不可降级）" : "";
        if(it->has_value) {
            fprintf(stderr, "[CONFIG] %s %s=%ld：期望 %s%s\n",
                    sev, it->field, it->value, it->rule ? it->rule : "", deg);
        }
        else {
            fprintf(stderr, "[CONFIG] %s %s：%s%s\n",
                    sev, it->field, it->rule ? it->rule : "", deg);
        }
    }
    if(r->total > r->count) {
        fprintf(stderr, "[CONFIG] ……其余 %d 项略\n", r->total - r->count);
    }
}

/* 把 value 钳到 [lo, hi]，返回值是否变化。 */
static bool clamp_int(int * v, int lo, int hi)
{
    if(*v < lo) { *v = lo; return true; }
    if(*v > hi) { *v = hi; return true; }
    return false;
}

int app_config_clamp(app_config_t * cfg)
{
    if(cfg == NULL) return 0;
    int n = 0;

    if(clamp_int(&cfg->log_max_entries, CFG_LOG_MAX_MIN, CFG_LOG_MAX_MAX)) n++;
    if(clamp_int(&cfg->face_otp_after, CFG_FACE_OTP_MIN, CFG_FACE_OTP_MAX)) n++;
    if(clamp_int(&cfg->face_verify_timeout_s, CFG_FACE_TIMEOUT_MIN, CFG_FACE_TIMEOUT_MAX)) n++;
    if(clamp_int(&cfg->max_failed, CFG_MAX_FAILED_MIN, CFG_MAX_FAILED_MAX)) n++;
    if(clamp_int(&cfg->lock_seconds, CFG_LOCK_SECONDS_MIN, CFG_LOCK_SECONDS_MAX)) n++;

    if(n > 0) {
        fprintf(stderr,
                "[CONFIG] 已钳制 %d 个越界字段：log_max=%d otp_after=%d timeout=%d "
                "max_failed=%d lock=%d\n",
                n, cfg->log_max_entries, cfg->face_otp_after,
                cfg->face_verify_timeout_s, cfg->max_failed, cfg->lock_seconds);
    }
    return n;
}

bool app_config_strict(void)
{
    const char * v = getenv("SAFE_CONFIG_STRICT");
    /* 缺省严格：生产设备宁可起不来（有日志、有 supervisor 限速），
     * 也不要带着坏配置静默运行。PC 调试显式设 0。 */
    if(v == NULL || *v == '\0') return true;
    return strcmp(v, "0") != 0;
}
