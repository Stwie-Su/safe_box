/**
 * @file hal_watchdog.h
 * 看门狗：程序卡死了，让它自动重启。分成三层，由外到内：
 *
 *   ① supervisor 进程（外面那个守护者）
 *        —— 读心跳文件，超时没更新就杀掉并重启应用；
 *   ② 软狗（写心跳文件 /var/run/...）
 *        —— 主循环每转一圈写一次，卡死就停写，交给 ① 处理；
 *   ③ 硬件看门狗（/dev/watchdog，i.MX6ULL 内置）
 *        —— 主循环也要喂它，停了 60 秒就**复位整块板子**。
 *           这是 supervisor 自己也挂了时的最后一层。
 *
 * 为什么喂狗放在主循环（main.c 的 while 里）：
 *   如果单独开一条线程无条件喂狗，主循环卡死了它照样喂，看门狗就形同虚设。
 *   只有「主循环真的跑完一圈」才喂，才说明程序还活着。
 *
 * 三层各管什么：
 *   软狗管「应用卡死 / 死循环」——重启应用，几秒钟恢复；
 *   硬狗管「内核死机 / supervisor 也挂了」——复位整机，几十秒恢复。
 *
 * 使用顺序（main.c）：init → start → 主循环里反复 kick → 退出前 stop。
 * ★ 退出前一定要 stop：不 stop 直接关程序，硬件狗会以为程序死了，把整机复位。
 */
#pragma once

#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件狗的超时时间，默认 60 秒。
 * 为什么这么长：硬件狗一响就是整机复位，启动稍慢就会被反复重启。
 * 「应用卡死」已经由软狗（5 秒判定）处理了，硬件狗只需要兜底更严重的情况。
 * 可以用环境变量 SAFE_WATCHDOG_TIMEOUT_MS 改。 */
#define HAL_WATCHDOG_DEFAULT_TIMEOUT_MS 60000u

/* 选后端、打开看门狗设备、设定超时。**还不开始喂**。
 * 在 app_main() 之前调，这样后面初始化卡住也能被发现。
 * 打开失败会自动降级：真狗 → 软狗 → 空实现，不会让程序起不来。 */
safe_err_t hal_watchdog_init(uint32_t timeout_ms);

/* 开始计时，并立刻喂第一口。init 之后调一次。 */
safe_err_t hal_watchdog_start(void);

/* 喂狗。main.c 主循环每转一圈调一次（内部按 1 秒节流，20ms 调一次也不会累）。
 * 没 init / start 时返回 SAFE_ERR_STATE。 */
safe_err_t hal_watchdog_kick(void);

/* 停止计时（真狗写 magic close）。退出链里调，漏了会被误复位。 */
safe_err_t hal_watchdog_stop(void);

/* 关设备、放资源。最后调。 */
void hal_watchdog_deinit(void);

/* 实际喂了多少次（状态页显示 / 自测用）。 */
uint32_t hal_watchdog_kick_count(void);

#ifdef __cplusplus
}
#endif
