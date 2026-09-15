/**
 * @file cred_reconcile.h
 * FR-21 防线 3「启动对账」：孤儿凭据标失效（需求 §5）。
 *
 * 模组用户清单首次取得后（face_service_module_users() 返回 >= 0）跑一次，
 * 把「本地有、模组无」的绑定凭据标记失效（face_enable=false，不删用户），
 * 且不用于放行；PIN / 动态码认证通道不受影响。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 纯比对 + 落盘：把模组侧清单（mod_ids / mod_count）与本地 users.json 比对，
 * 本地 face_id >= 0 且不在清单里的用户 → face_enable=false + 审计日志（"ALARM"）。
 *   - face_id == -1（未绑定人脸）不是孤儿，不许动；
 *   - 已失效（face_enable == false）的用户不重复动作（幂等）；
 *   - mod_count < 0（-1 无此概念 / -2 尚未取得）直接返回 0，不误杀全部凭据。
 * 返回被标失效的用户数（>= 0）。
 *
 * ⚠️ 本函数会同步读写 users.json —— 生产路径【必须】在 worker 线程调用
 *   （由 cred_reconcile_tick() 经 worker_post 派发，守「认证路径零同步落盘」纪律）；
 *   仅单元测试在主线程直接调用。 */
int cred_reconcile_apply(const int32_t * mod_ids, int32_t mod_count);

/* 主线程 20ms tick 驱动：模组清单首次取得后触发一次后台对账，且只触发一次（幂等）。
 * 清单未取得（-2）或无此概念（-1）时不触发。文件 IO 经 worker_post 下沉后台线程。 */
void cred_reconcile_tick(void);

/* 测试用：清除「已对账」标志，使下次 tick 可再次触发。 */
void cred_reconcile_reset(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
