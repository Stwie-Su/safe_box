/**
 * @file store_cache.h
 * 用户表的内存快照：主线程想「查个用户」时不用读盘。
 *
 * 为什么需要：读写 users.json 只能由 worker 线程做，但界面经常要查
 * 「这个 id 叫什么名字」「一共几个用户」。每次都投一个后台作业太重，
 * 所以 worker 每次写完盘顺便更新一份内存快照，主线程直接读这份。
 *
 * 怎么保证读不到「改了一半」的数据：准备两块数组，worker 只写空闲的那块，
 * 写完整块再切换「当前有效」标记；读的一方先看标记、再整块拷走。
 * 单核 CPU + 只有一个 worker 线程的前提下，这样不加锁也不会读到半成品。
 *
 * ★ 代价要说清楚：快照是「上一次写盘之后」的状态，可能慢一拍。
 *   所以凡是会改变安全结论的判断（失败次数、锁定时间、已用次数），
 *   都不能基于快照做「读出来改一改写回去」，那些必须交给 async_store 的
 *   astore_auth_side 整段在 worker 里做。这里只适合查「有没有这个人」
 *   「通道开没开」「他是什么角色」这类慢一拍也不影响结论的信息。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 快照最多放多少个用户（静态数组，不占堆）。超出的部分会被丢掉。 */
#define STORE_CACHE_USER_MAX 64

/* 写快照：**只能 worker 线程调**（一般在每次写盘成功后）。
 * list 可以传 NULL（等于发布一份空表）；count 会被夹到 [0, STORE_CACHE_USER_MAX]。 */
void store_cache_publish_users(const safe_user_t *list, int count);

/* 读：有几个用户 / 快照版本号。两个出参都可以传 NULL。
 * 版本号每次发布 +1；**0 表示还没发布过**（这时数量是 0，但意思是「还不知道」
 * 而不是「确实没有用户」）。 */
int store_cache_user_count(int *count, uint32_t *stamp);

/* 读：按 id 查。找到返回 0 并写进 *out；没找到返回 -1。out 传 NULL 只判断存在性。 */
int store_cache_find_by_id(int id, safe_user_t *out);

/* 读：按用户名查（认证路径用）。0 = 找到。不读盘，数据来自最新快照。 */
int store_cache_find_by_name(const char *name, safe_user_t *out);

/* 读：按人脸模板号查。0 = 找到。同样不读盘。 */
int store_cache_find_by_face(int face_id, safe_user_t *out);

#ifdef __cplusplus
} /*extern "C"*/
#endif
