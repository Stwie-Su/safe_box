/**
 * @file cred_reconcile.h
 * 开机对账：检查「本地记着的人脸」在模组里还在不在。
 *
 * 为什么需要：模组可能被人整个清空、或者换了模组，而本地 users.json 还记着
 * 「这个用户绑了 3 号人脸」。这时如果不检查，就会出现「本地以为有脸、实际刷不出来」。
 *
 * 做法：模组的用户清单第一次拿到后，把「本地有、模组没有」的绑定向标成失效
 * （face_enable=false，不删用户），PIN 和动态码照常能用。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 比对并落盘，返回标失效的用户数。
 * 这条会读写 users.json，所以必须在 worker 线程跑（cred_reconcile_tick 已经帮你安排了）；
 * 只有单元测试会在主线程直接调。
 *   - face_id == -1 表示没绑人脸，不算孤儿；
 *   - 已经失效的不重复处理；
 *   - mod_count < 0（模组不提供清单 / 还没拿到）时直接返回 0，避免把所有人都判失效。 */
int cred_reconcile_apply(const int32_t * mod_ids, int32_t mod_count);

/* 主线程每 20ms 调一次。清单拿到后触发一次对账，只触发一次。 */
void cred_reconcile_tick(void);

/* 测试用：清掉「已经对过账」的标志，让下次 tick 能再来一遍。 */
void cred_reconcile_reset(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
