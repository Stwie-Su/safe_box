/**
 * @file ota_status.h
 * OTA 状态文件（UI 与升级脚本之间的跨进程 IPC 数据格式约定）的纯解析层。
 *
 * 文件格式：KEY=VALUE 每行一个，'\n' 分隔（容忍 '\r\n'），无尾换行亦可。
 *
 * 约定要点（2026-10-02「检查中…」卡死复盘固化，D19）：
 *   - 写方（ota_check.sh / ota_apply.sh）以「重写式」维护当前状态快照；
 *   - 历史上出现过「追加式」写法（CHECKING→CHECKED 逐行追加），同名 KEY
 *     因此可能多次出现 —— 读方必须取**最后一次**匹配（终态总在尾部）。
 *     取首个匹配的实现在追加式写法下永远读到旧状态，UI 永远转圈。
 *   - 因此本解析层与写方约定解耦：无论追加式还是重写式，语义都正确。
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 在状态文本里按键取值（取最后一次出现）。
 *
 * @param buf  状态文件全文（可为 NULL）
 * @param key  键名（不含 '='）
 * @param out  输出缓冲
 * @param cap  缓冲容量（含 NUL）；值超长时**截断**并返回 0
 * @return 0 找到；-2 参数非法；-4 未找到
 */
int ota_status_query(const char * buf, const char * key, char * out, size_t cap);

#ifdef __cplusplus
}
#endif
