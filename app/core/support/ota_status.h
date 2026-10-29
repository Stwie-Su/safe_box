/**
 * @file ota_status.h
 * 读 OTA 升级状态文件（UI 和升级脚本之间靠这个文件传递进度）。
 *
 * 文件格式：一行一个 KEY=VALUE，例如
 *   STATE=CHECKING
 *   PROGRESS=40
 *
 * 注意：同名 KEY 可能出现多次（脚本是追加写的），读的时候取**最后一次**，
 * 因为最新状态总写在文件末尾。
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 按键取值，取最后一次出现的那个。
 * 返回 0 = 找到；-2 = 参数不对；-4 = 没有这个键。值太长会被截断（仍返回 0）。 */
int ota_status_query(const char * buf, const char * key, char * out, size_t cap);

#ifdef __cplusplus
}
#endif
