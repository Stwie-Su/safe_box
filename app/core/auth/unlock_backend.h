/**
 * @file unlock_backend.h
 * 解锁后端接口（策略抽象）：UI / FSM 只调用本接口，不持有任何判定逻辑。
 * 多用户模型（DESIGN.md §2）：三通道（PIN / 人脸 / 动态码）共用同一张用户表。
 *
 * 阶段 1 新增：auth_result_t 统一返回码 + 动态码校验入口（FR-1 / NFR-3）。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 三通道 / 远程开锁的统一返回码（TOTP / 远程开锁 / 管理员二次确认） */
typedef enum {
    AUTH_OK = 0,        /* 验证通过 */
    AUTH_FAIL,          /* 验证失败（未达锁定） */
    AUTH_LOCKED,        /* 相关用户处于锁定 */
    AUTH_DISABLED,      /* 用户已停用 */
    AUTH_NOUSER,        /* 用户不存在 */
    AUTH_EXPIRED,       /* 临时授权已过期 / 次数用尽 */
    AUTH_CHANNEL_OFF,   /* 该功能未启用（face_enable / totp_enable 为 false） */
    AUTH_REPLAY,        /* 动态码已使用（防重放） */
} auth_result_t;

/* PIN 主页开锁专用返回码（与 baseline 兼容：page_keypad.c 仍按此判定） */
typedef enum {
    UNLOCK_OK = 0,      /* 验证通过 */
    UNLOCK_FAIL,        /* 验证失败（未达锁定） */
    UNLOCK_LOCKED       /* 所有启用用户均处于锁定 */
} unlock_result_t;

/* PIN 主页开锁（**开锁通道专用**）：匹配任一【启用且未锁定】用户，成功时 out_user 填用户名。
 * 是否接受虚位密码（FR-18）由策略开关 virtual_pin_enable 决定。
 * ⚠ 敏感操作的管理员二次确认**不要**用本接口（它会跟着开关走虚位），用 backend_verify_admin_pin。 */
unlock_result_t backend_verify_pin(const char *pin, char *out_user, size_t user_cap);

/* 同上，但由调用方显式指定是否允许虚位（供远程开锁等需要覆盖策略的通道使用）。 */
unlock_result_t backend_verify_pin_ex(const char *pin, char *out_user, size_t user_cap,
                                      bool allow_virtual);

/* 管理员二次确认 PIN（FR-14 / 规约 §5.5）：**恒为精确匹配**，不看虚位开关。
 * 依据：规约 §5.5「管理员二次确认：不支持虚位，必须精确匹配——敏感操作不允许以降低
 * 精度换取便利」。实现上等价于 backend_verify_pin_ex(..., false)。 */
unlock_result_t backend_verify_admin_pin(const char *pin, char *out_user, size_t user_cap);

/* 动态码开锁：校验某用户的 TOTP（含 ±1 窗口容忍 + 防重放 + 失败计数 + 通道开关）。 */
auth_result_t backend_verify_totp(const char *user, const char *code);

/* 身份未定动态码（人脸连续 NO_MATCH 转入 WAIT_OTP 时 pending_user 为空）：
 * 在「任一启用 + TOTP 开启 + 未锁定 + 临时授权有效」的用户上校验 code。
 * 成功时 out_user 填命中用户名。失败不累加用户级计数（防误伤），由 FSM 设备级计数兜底。 */
auth_result_t backend_verify_totp_any(const char *code, char *out_user, size_t user_cap);

/* 管理员动态码二次确认：在「任一启用且开启 TOTP 的管理员」上验证 otp。
 * 用于 FR-3 敏感操作 / 远程开锁强制 TOTP。成功返回 AUTH_OK。 */
auth_result_t backend_admin_verify_totp(const char *code);

/* 是否存在处于锁定的启用用户（任一通道锁定也算） */
bool backend_is_locked(void);

/* 所有用户中最大锁定剩余秒数（0 = 无锁定） */
int backend_lock_remaining(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

