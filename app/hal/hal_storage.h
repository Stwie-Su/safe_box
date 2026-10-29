/**
 * @file hal_storage.h
 * 查磁盘用了多少（主页「存储占用」显示用）。
 *
 * 界面层不要直接调 statvfs()：那是平台相关的接口，混进界面层以后换平台要到处改。
 * 路径由调用方传进来 —— 这一层不知道「数据目录在哪」，那是业务层的知识。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* path 所在分区已用百分比（0~100）。
 * 路径为空或查不到时返回 -1，界面自行决定怎么显示（当前按 0% 处理）。 */
int hal_storage_used_pct(const char * path);

#ifdef __cplusplus
}
#endif
