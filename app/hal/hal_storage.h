/**
 * @file hal_storage.h
 * 存储容量抽象（监控页「存储占用」用）。
 *
 * 业务层（尤其 ui/）一律走本接口，不直接调 statvfs(3)：
 *   - ui 层不得出现平台实现头（<sys/statvfs.h>），分层检查（tools/check_layers.sh）禁止；
 *   - 目标路径由调用方传入 —— hal 层不得反向依赖 core（拿不到 store_dir()），
 *     「数据目录在哪」是业务知识，由 ui 从 core.store 取来再传给本接口。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* path 所在文件系统的「已用百分比」（整数 0..100）。
 * path 为 NULL / 空 / 无法查询（statvfs 失败、f_blocks==0）时返回 -1；
 * 调用方自行决定如何呈现「未知」（当前监控页按 0% 显示）。 */
int hal_storage_used_pct(const char * path);

#ifdef __cplusplus
}
#endif
