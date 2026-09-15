/**
 * @file storage_service.c
 * 存储容量门面：把 statvfs(3) 收进 hal，供 ui 层无平台依赖地查询。
 *
 * 为什么单独一层：UI 监控页要显示真实文件系统使用率，但 ui/ 不得直接 include
 * <sys/statvfs.h>（平台实现头，tools/check_layers.sh 禁止）。目标路径由调用方
 * 传入 —— hal 不反向依赖 core 的 store_dir()（那是 core 的知识）。
 */

#include "hal/hal_storage.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/statvfs.h>

int hal_storage_used_pct(const char * path)
{
    if(path == NULL || *path == '\0') return -1;

    struct statvfs vfs;
    if(statvfs(path, &vfs) != 0) return -1;
    if(vfs.f_blocks == 0) return -1;

    /* 已用块占比：f_blocks - f_bfree 把 root 保留块也算「已用」，与 df 口径一致。 */
    uint64_t used = (uint64_t)(vfs.f_blocks - vfs.f_bfree);
    uint64_t pct  = (used * 100u) / (uint64_t)vfs.f_blocks;
    if(pct > 100u) pct = 100u;
    return (int)pct;
}
