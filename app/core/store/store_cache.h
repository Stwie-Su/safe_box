/**
 * @file store_cache.h
 * 【轻量版】只读用户表快照缓存（QA-20 不变式的配套设施）。
 *
 * 背景：不变式升级为「users.json / safe.log 的**读与写**全部由 worker 线程独占」
 * 之后，主线程就不能再调 `user_load_all()` 了 —— 但 `build_status()` 想要
 * `user_count`、UI 想要按 id 找用户，为这种小查询每次投一个 worker 作业太重。
 * 于是给一份「worker 写、主线程只读」的快照缓存。
 *
 * 并发模型（**迷你版 RCU**）：
 *   两块定长 `safe_user_t[]` 双缓冲 + 一个 `volatile int active` 指示当前有效块。
 *   写侧（worker）永远只填 **inactive** 块，填完再切 `active`；
 *   读侧（主线程）先取 active、再 memcpy 一份出来用。
 *   → 读侧永不会读到「填了一半」的块，也不需要加锁、不会阻塞。
 *
 * ★ 诚实边界（面试要主动说）：`volatile` 不是 C11 的原子语义，严格讲不足以保证
 *   多核上的可见性顺序。本项目目标机是**单核** Cortex-A7，且写侧恒为唯一一个
 *   worker 线程（不变式保证），这个前提下它是充分的；若将来上多核，应把
 *   `active` 换成 `_Atomic int` 并给写侧加 release / 读侧加 acquire。
 *
 * 为什么**不做**全量缓存：只缓存「用户表」这一个高频只读对象，不缓存日志、
 * 不缓存策略（策略已由 `user_policy()` 的加锁快照解决）。范围越小越不容易腐化。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 快照容量：与 users.json 的用户数上限对齐（静态分配，**不做堆分配**）。
 * 超出部分被截断（写侧静默丢弃尾部），读侧最多看到这么多条。 */
#define STORE_CACHE_USER_MAX 64

/**
 * 写侧接口：**只能由 worker 线程调用**（通常在每次落盘成功后）。
 * list 可为 NULL（等价于发布一份「空表」）；count 会被钳到 [0, STORE_CACHE_USER_MAX]。
 */
void store_cache_publish_users(const safe_user_t *list, int count);

/**
 * 读侧接口：主线程 / 任何线程均可调用，不落盘、不阻塞、不投作业。
 * count 与 stamp 均可为 NULL（只想要其中一个时传 NULL 即可）。
 * stamp = 快照版本号，每次发布 +1；**stamp == 0 表示从未发布过**（此时 count 为 0，
 * 调用方应把它理解为「未知」而不是「确实没有用户」）。
 * 返回 0 = 成功（当前实现恒为 0，保留返回值以便将来区分错误）。
 */
int store_cache_user_count(int *count, uint32_t *stamp);

/**
 * 读侧接口：按 id 查快照。找到返回 0 并拷进 *out；未找到返回 -1。
 * out 为 NULL 时只做「是否存在」的判断。
 */
int store_cache_find_by_id(int id, safe_user_t *out);

#ifdef __cplusplus
} /*extern "C"*/
#endif
