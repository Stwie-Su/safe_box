/**
 * @file err.h
 * 全项目通用的错误码。
 *
 * 约定：0 = 成功，负数 = 失败。认证模块用自己的 auth_result_t（语义更细，
 * 要映射成远程指令的错误码），其余模块一律用这里的枚举。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAFE_OK          =   0,   /* 成功 */
    SAFE_ERR_FAIL    =  -1,   /* 通用失败 */
    SAFE_ERR_PARAM   =  -2,   /* 参数不合法 */
    SAFE_ERR_NOMEM   =  -3,   /* 内存不够 */
    SAFE_ERR_NOENT   =  -4,   /* 记录不存在 */
    SAFE_ERR_EXIST   =  -5,   /* 记录已存在 */
    SAFE_ERR_PERM    =  -6,   /* 权限不足 */
    SAFE_ERR_IO      =  -7,   /* 文件读写失败 */
    SAFE_ERR_LOCKED  =  -8,   /* 处于锁定状态 */
    SAFE_ERR_EXPIRED =  -9,   /* 已过期或次数用完 */
    SAFE_ERR_BUSY    = -10,   /* 设备忙（比如正在录人脸） */
    SAFE_ERR_UNSUP   = -11,   /* 当前后端不支持这个操作 */
    SAFE_ERR_TIMEOUT = -12,   /* 超时 */
    SAFE_ERR_STATE   = -13,   /* 调用时序不对（例如还没 init 就用） */
} safe_err_t;

/* 错误码转成可读字符串，打日志用。 */
const char * safe_err_str(safe_err_t e);

#ifdef __cplusplus
}
#endif
