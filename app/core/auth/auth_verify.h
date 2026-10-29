/**
 * @file auth_verify.h
 * 认证判定：比对 PIN、动态码，判断用户能不能通过。
 *
 * 界面和状态机都只调这里，不自己写判断逻辑。三个通道（PIN / 人脸 / 动态码）
 * 共用同一张用户表。
 *
 * 注意：本文件的函数会读盘，属于「慢操作」。
 *   - 界面要用 PIN 校验时，走 astore_verify_admin / auth_fsm_submit_pin（后台线程）；
 *   - 不要在主线程直接调 auth_verify_pin，会卡界面。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一返回码：动态码、远程开锁、管理员二次确认都用它。 */
typedef enum {
    AUTH_OK = 0,        /* 通过 */
    AUTH_FAIL,          /* 失败（还没到锁定） */
    AUTH_LOCKED,        /* 该用户被锁定 */
    AUTH_DISABLED,      /* 用户已停用 */
    AUTH_NOUSER,        /* 没这个用户 */
    AUTH_EXPIRED,       /* 临时授权过期 / 次数用完 */
    AUTH_CHANNEL_OFF,   /* 这个通道没开（face_enable / totp_enable 为 false） */
    AUTH_REPLAY,        /* 这个动态码已经用过了 */
} auth_result_t;

/* PIN 开锁专用返回码，界面按这三个分支显示提示。 */
typedef enum {
    UNLOCK_OK = 0,      /* 通过 */
    UNLOCK_FAIL,        /* 失败 */
    UNLOCK_LOCKED       /* 所有启用用户都在锁定中 */
} unlock_result_t;

/* 主页键盘开锁：只要匹配到任一「启用且未锁定」的用户就算通过，用户名写进 out_user。
 * 是否接受虚位密码由安全策略决定。 */
unlock_result_t auth_verify_pin(const char *pin, char *out_user, size_t user_cap);

/* 同上，但由调用方决定这次允不允许虚位密码（远程开锁等场景用）。 */
unlock_result_t auth_verify_pin_ex(const char *pin, char *out_user, size_t user_cap,
                                      bool allow_virtual);

/* 管理员二次确认用的 PIN 校验：**必须精确匹配**，不认虚位密码。
 * 敏感操作（改密码、进设置）用这个，不要用上面的。 */
unlock_result_t auth_verify_admin_pin(const char *pin, char *out_user, size_t user_cap);

/* 校验指定用户的动态码（允许前后各一个时间片，防止手机和板子差几秒；同一时间片只能用一次）。 */
auth_result_t auth_verify_totp(const char *user, const char *code);

/* 不知道是谁的时候校验动态码（人脸连续没认出来、转入输码时会这样）：
 * 在「启用 + 开了动态码 + 未锁定 + 临时授权有效」的用户里逐个试。
 * 成功时输出命中的用户名；失败不加用户失败次数（避免误伤别人）。 */
auth_result_t auth_verify_totp_any(const char *code, char *out_user, size_t user_cap);

/* 管理员动态码二次确认（敏感操作 / 远程开锁强制要求）。 */
auth_result_t auth_admin_verify_totp(const char *code);

/* 现在有没有启用中的用户在锁定状态。 */
bool auth_is_locked(void);

/* 所有用户里最长的锁定剩余秒数（0 = 没锁）。 */
int auth_lock_remaining(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
