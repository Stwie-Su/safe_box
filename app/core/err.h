/**
 * @file err.h
 * 统一错误码。
 *
 * 背景：此前存储层用 0 成功/-1 失败，认证域用自己的枚举，部分函数返回 -1 三种含义。
 * 除认证域（auth_result_t，语义域不同且映射 RPC 错误码）外，模块间统一用本枚举。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAFE_OK          =   0,   /* 成功 */
    SAFE_ERR_FAIL    =  -1,   /* 通用失败 */
    SAFE_ERR_PARAM   =  -2,   /* 参数非法 */
    SAFE_ERR_NOMEM   =  -3,   /* 内存不足 */
    SAFE_ERR_NOENT   =  -4,   /* 记录不存在 */
    SAFE_ERR_EXIST   =  -5,   /* 记录已存在 */
    SAFE_ERR_PERM    =  -6,   /* 权限不足 */
    SAFE_ERR_IO      =  -7,   /* 文件读写失败 */
    SAFE_ERR_LOCKED  =  -8,   /* 处于锁定状态 */
    SAFE_ERR_EXPIRED =  -9,   /* 已过期或次数用尽 */
    SAFE_ERR_BUSY    = -10,   /* 设备忙（如正在录入人脸） */
    SAFE_ERR_UNSUP   = -11,   /* 当前后端不支持该操作 */
    SAFE_ERR_TIMEOUT = -12,   /* 超时 */
    SAFE_ERR_STATE   = -13,   /* 状态机/调用时序错误 */
} safe_err_t;

const char * safe_err_str(safe_err_t e);

#ifdef __cplusplus
}
#endif
