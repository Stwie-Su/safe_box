/**
 * @file config_validate.h
 * 配置校验：把「配错了也能跑」变成「配错了能被发现」。
 *
 * 【为什么需要独立一层】
 *   config.c 只负责把环境变量与策略合成快照，不做任何判断。此前的结果是：
 *   端口写成 70000、TLS 开了却没给 CA、数据目录不可写 —— 全部静默进入运行期，
 *   表现成「远程连不上」「存不进用户」这类与配置无关的故障，排查要绕一大圈。
 *
 * 【分级策略（失效要导向安全侧，但不能一刀切拒绝启动）】
 *   FATAL：无法安全运行，或继续跑会制造「以为安全其实不安全」的假象
 *          （TLS 要求校验却没有 CA、数据目录不可写）→ 严格模式拒绝启动。
 *   WARN ：可钳制到合法范围（阈值越界、日志条数超限）→ 记录并钳制，继续运行。
 *
 * 【严格模式】
 *   SAFE_CONFIG_STRICT=1（缺省）→ FATAL 即拒绝启动；板端生产用这个。
 *   SAFE_CONFIG_STRICT=0         → FATAL 降级：关闭远程通道（绝不退化成明文），
 *                                  其余钳制后继续；PC 开发调试用这个。
 *
 * 【可测试性】
 *   app_config_validate() 是纯函数：不读环境变量、不分配堆内存、不修改入参，
 *   只读文件系统判断路径可用性。测试可以直接构造 app_config_t 断言结论。
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
    CFG_SEV_WARN  = 0,   /* 可钳制 / 可降级 */
    CFG_SEV_FATAL = 1,   /* 严格模式下拒绝启动 */
} cfg_severity_t;

typedef struct {
    cfg_severity_t severity;
    const char *   field;    /* 字段名，便于 grep 与运维定位 */
    bool           has_value;/* true = value 有意义（数值字段） */
    bool           degradable;/* FATAL 时：true = 可降级（关远程通道），false = 必须拒绝启动 */
    long           value;    /* 数值现场值；非数值字段填 0 */
    const char *   rule;     /* 期望范围 / 约束 */
} cfg_issue_t;

typedef struct {
    cfg_issue_t items[CFG_MAX_ISSUES];
    int         count;       /* 已记录条数（<= CFG_MAX_ISSUES） */
    int         total;       /* 实际发现问题总数（可能大于 count） */
    int         fatal_count;
    int         fatal_blocking; /* 其中不可降级的（数据目录不可写等） */
    int         warn_count;
} cfg_report_t;

/* ------------------------------------------------------------------
 * 函数：app_config_validate
 * 功能：按「范围 → 字符串 → 路径 → 组合约束」四类规则检查配置快照。
 * 调用者：app_config_init()（启动时一次）；单元测试（直接构造结构体）。
 * 数据流：只读 cfg；结果写入 out（先清零）。
 * 说明：不修改 cfg —— 钳制是调用方的显式动作，见 app_config_clamp()。
 * ------------------------------------------------------------------ */
void app_config_validate(const app_config_t * cfg, cfg_report_t * out);

/* ------------------------------------------------------------------
 * 函数：app_config_report
 * 功能：把报告打到 stderr，一行一条，前缀 [CONFIG] 便于 grep / 日志采集。
 * 调用者：app_config_init()。
 * ------------------------------------------------------------------ */
void app_config_report(const cfg_report_t * r);

/* ------------------------------------------------------------------
 * 函数：app_config_clamp
 * 功能：把 WARN 级数值字段钳制到合法范围（FATAL 字段不在这里处理）。
 * 调用者：app_config_init()（记录报告之后）。
 * 返回值：被修改的字段数。
 * 说明：只改快照镜像；users.json 里的真实策略值由 store 读盘时钳制。
 * ------------------------------------------------------------------ */
int app_config_clamp(app_config_t * cfg);

/* ------------------------------------------------------------------
 * 函数：app_config_strict
 * 功能：读 SAFE_CONFIG_STRICT，缺省 true（生产安全侧）。
 * 调用者：app_config_init()。
 * ------------------------------------------------------------------ */
bool app_config_strict(void);

/* 字段合法范围（唯一来源：config_validate.c 与单元测试共用，避免两处写两个数）。
 * log 下限取 1 而不是 50：保留条数只影响磁盘占用，不构成安全边界；
 * 测试与排障会用 SAFE_LOG_MAX=3 这类小值验证滚动，钳到 50 会把测试语义改掉。 */
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
