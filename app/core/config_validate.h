/**
 * @file config_validate.h
 * 配置检查：把「配错了也能跑」变成「配错了能立刻发现」。
 *
 * 以前端口写成 70000、开了 TLS 却没给 CA、数据目录不可写，全都没人管，
 * 最后表现成「远程连不上」「用户存不进去」这种看起来和配置无关的故障。
 *
 * 检查结果分两级：
 *   FATAL —— 继续跑会造成「以为安全其实不安全」（比如要求校验证书却没给 CA），
 *            严格模式下直接拒绝启动；
 *   WARN  —— 能自动改到合法范围（阈值越界、日志条数太大），
 *            记一行日志、改小继续跑。
 *
 * 严格模式由环境变量 SAFE_CONFIG_STRICT 控制，默认开（生产用）；
 * PC 调试时设成 0，FATAL 会降级成「关掉远程通道」而不是退出。
 */
#pragma once

#include <stdbool.h>

#include "core/config.h"
#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_ISSUES 16

typedef enum {
    CFG_SEV_WARN  = 0,   /* 可以钳制 / 可以降级 */
    CFG_SEV_FATAL = 1,   /* 严格模式下拒绝启动 */
} cfg_severity_t;

typedef struct {
    cfg_severity_t severity;
    const char *   field;    /* 哪个字段出的问题，方便 grep */
    bool           has_value;/* true = value 有意义（数值字段） */
    bool           degradable;/* FATAL 时：true = 可以降级（关远程通道），false = 必须拒绝启动 */
    long           value;    /* 出问题时的实际值；非数值字段填 0 */
    const char *   rule;     /* 期望的范围 / 约束 */
} cfg_issue_t;

typedef struct {
    cfg_issue_t items[CFG_MAX_ISSUES];
    int         count;       /* 已记录条数（<= CFG_MAX_ISSUES） */
    int         total;       /* 实际发现的问题总数（可能大于 count） */
    int         fatal_count;
    int         fatal_blocking; /* 其中不能降级的（数据目录不可写等） */
    int         warn_count;
} cfg_report_t;

/* 检查一遍配置（范围 → 字符串 → 路径 → 组合约束），结果写进 out（会先清零）。
 * 只读 cfg，不改它；要改是下一步 clamp 的事。 */
void app_config_validate(const app_config_t * cfg, cfg_report_t * out);

/* 把检查报告打到 stderr，一行一条，前缀 [CONFIG]，方便 grep。 */
void app_config_report(const cfg_report_t * r);

/* 把 WARN 级的数值字段改到合法范围。返回改了几个字段。
 * 只改内存里的这份快照；users.json 里的真实策略由 store 读盘时自己钳制。 */
int app_config_clamp(app_config_t * cfg);

/* 读环境变量 SAFE_CONFIG_STRICT，没设时默认 true（往安全的一侧靠）。 */
bool app_config_strict(void);

/* 字段合法范围（唯一来源，测试也用它，避免两处写两个数）。
 * 日志下限取 1 而不是 50：条数只影响磁盘占用，不涉及安全；
 * 测试和排障会用 SAFE_LOG_MAX=3 这种小值验证滚动，钳到 50 会把测试语义改掉。 */
#define CFG_LOG_MAX_MIN          1
#define CFG_LOG_MAX_MAX          100000
#define CFG_FACE_OTP_MIN         1
#define CFG_FACE_OTP_MAX         10
#define CFG_FACE_TIMEOUT_MIN     3
#define CFG_FACE_TIMEOUT_MAX     120
#define CFG_MAX_FAILED_MIN       1
#define CFG_MAX_FAILED_MAX       10
#define CFG_LOCK_SECONDS_MIN     5
#define CFG_LOCK_SECONDS_MAX     3600

#ifdef __cplusplus
}
#endif
