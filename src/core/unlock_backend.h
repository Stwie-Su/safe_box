/**
 * @file unlock_backend.h
 * 解锁后端接口（策略抽象）：UI 只调用本接口，不持有任何判定逻辑。
 * 多用户模型（DESIGN.md §2）：PIN 匹配任一启用用户；防暴力按用户独立计数。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UNLOCK_OK,      /* 验证通过 */
    UNLOCK_FAIL,    /* 验证失败（未达锁定） */
    UNLOCK_LOCKED   /* 所有启用用户均处于锁定 */
} unlock_result_t;

/* 主页开锁：PIN 匹配任一启用用户。成功时 out_user 填用户名（含 \0）。 */
unlock_result_t backend_verify_pin(const char *pin, char *out_user, size_t user_cap);

/* 是否存在处于锁定的启用用户 */
bool backend_is_locked(void);

/* 所有用户中最大锁定剩余秒数（0 = 无锁定） */
int backend_lock_remaining(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
