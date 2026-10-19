/**
 * @file main.c
 * 进程入口：只做引导与主循环，不含任何业务逻辑或平台判断。
 *
 * 顺序：平台引导（命令行 + 后端注册）→ lv_init → 显示输入初始化 → 应用启动 → 主循环。
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lvgl.h"

#include "app.h"
#include "app_version.h"
#include "hal/hal_watchdog.h"
#include "platform/debug_hooks.h"
#include "platform/perf_probe.h"
#include "platform/platform.h"

static void timer_fast(lv_timer_t * t)      { (void)t; app_tick_fast(); }
static void timer_slow(lv_timer_t * t)      { (void)t; app_tick_slow(); }
static void timer_periodic(lv_timer_t * t)  { (void)t; app_tick_periodic(); }

/* 优雅退出（规约 §3.2）：信号处理函数内**只置标志**，不做任何 IO / 分配 / printf。 */
static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

int main(int argc, char ** argv)
{
    platform_bootstrap(argc, argv);

    /* 行缓冲：被 supervisor 守护时 stdout 是文件（默认全缓冲），
     * 进程被 SIGSEGV / SIGKILL 时缓冲区里最后几 KB 日志会丢；
     * 行缓冲把丢日志的窗口压到一行以内，排障时这几行往往就是现场。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* 注册 SIGTERM / SIGINT：处理函数内只置 g_quit（§3.2）。 */
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    lv_init();

    if(platform_init_io() != 0) {
        fprintf(stderr, "[main] 显示后端初始化失败\n");
        return 1;
    }

    perf_probe_init_from_env();

    /* 看门狗：开设备并设定喂狗超时（默认 15s，SAFE_WATCHDOG_TIMEOUT_MS 可覆盖）。
     * 排在 app_main() 之前——这样后续初始化若卡死也能被硬件复位；
     * 无 /dev/watchdog 的环境（PC）自动降级 null 后端，接口照常调用但不产生复位能力。 */
    {
        const char * wd = getenv("SAFE_WATCHDOG_TIMEOUT_MS");
        uint32_t wd_ms = (wd != NULL && *wd != '\0')
                             ? (uint32_t)strtoul(wd, NULL, 10)
                             : HAL_WATCHDOG_DEFAULT_TIMEOUT_MS;
        hal_watchdog_init(wd_ms);
        hal_watchdog_start();
    }

    if(app_main() != 0) {
        /* 启动中止（典型原因：配置校验 FATAL + 严格模式）。
         * 必须走 stop（magic close）再退出：否则关闭 fd 后看门狗继续计时，
         * 15s 后整机复位 —— 表现为「应用起不来还反复重启整机」。 */
        fprintf(stderr, "[main] 启动中止，停止看门狗后退出\n");
        hal_watchdog_stop();
        hal_watchdog_deinit();
        return 2;
    }

    /* PC 调试钩子（指定起始页/主题/截图）。板子构建下为空实现。 */
    debug_hooks_apply();

    lv_timer_create(timer_fast, 20, NULL);
    lv_timer_create(timer_slow, 100, NULL);
    lv_timer_create(timer_periodic, 5000, NULL);

    printf("[main] safe %s (%s) 启动完成，进入主循环\n", SAFE_VERSION_STRING, SAFE_GIT_DESC);

    while(!g_quit) {
        uint32_t t0 = perf_probe_enabled() ? lv_tick_get() : 0;
        uint32_t ms = lv_timer_handler();
        if(perf_probe_enabled()) perf_probe_record(lv_tick_get() - t0);
        /* 心跳喂狗：只有主循环真正跑完这一拍才喂——卡死/死循环时停喂 → 硬件在超时后复位设备。
         * 分发层内部按 SAFE_WATCHDOG_KICK_MS（默认 1s）节流，20ms 一拍也不会把系统调用打满。 */
        hal_watchdog_kick();
        if(ms == LV_NO_TIMER_READY) ms = LV_DEF_REFR_PERIOD;
        usleep(ms * 1000);
    }

    /* 主循环检测 g_quit 跳出 -> 优雅退出（§3.2）。 */
    printf("[main] 收到退出信号，开始优雅退出\n");
    app_shutdown();

    /* 退出链最后一环：停止看门狗计时（magic close）——漏掉会让关闭/重启应用时被硬件误复位。 */
    hal_watchdog_stop();
    hal_watchdog_deinit();

    return 0;
}
