/**
 * @file main.c
 * 进程入口：只做引导与主循环，不含任何业务逻辑或平台判断。
 *
 * 顺序：平台引导（命令行 + 后端注册）→ lv_init → 显示输入初始化 → 应用启动 → 主循环。
 */

#include <stdio.h>
#include <unistd.h>

#include "lvgl.h"

#include "app.h"
#include "app_version.h"
#include "platform/debug_hooks.h"
#include "platform/perf_probe.h"
#include "platform/platform.h"

static void timer_fast(lv_timer_t * t)      { (void)t; app_tick_fast(); }
static void timer_slow(lv_timer_t * t)      { (void)t; app_tick_slow(); }
static void timer_periodic(lv_timer_t * t)  { (void)t; app_tick_periodic(); }

int main(int argc, char ** argv)
{
    platform_bootstrap(argc, argv);

    lv_init();

    if(platform_init_io() != 0) {
        fprintf(stderr, "[main] 显示后端初始化失败\n");
        return 1;
    }

    perf_probe_init_from_env();

    app_main();

    /* PC 调试钩子（指定起始页/主题/截图）。板子构建下为空实现。 */
    debug_hooks_apply();

    lv_timer_create(timer_fast, 20, NULL);
    lv_timer_create(timer_slow, 100, NULL);
    lv_timer_create(timer_periodic, 5000, NULL);

    printf("[main] safe %s (%s) 启动完成，进入主循环\n", SAFE_VERSION_STRING, SAFE_GIT_DESC);

    while(1) {
        uint32_t t0 = perf_probe_enabled() ? lv_tick_get() : 0;
        uint32_t ms = lv_timer_handler();
        if(perf_probe_enabled()) perf_probe_record(lv_tick_get() - t0);
        if(ms == LV_NO_TIMER_READY) ms = LV_DEF_REFR_PERIOD;
        usleep(ms * 1000);
    }

    return 0;
}
