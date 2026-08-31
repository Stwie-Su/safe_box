/**
 * @file platform_null.c
 * 无显示平台：defconfig 既没开 SDL 也没开 FBDEV 时的兜底，保证工程仍能编译运行。
 */

#include "platform/platform.h"

#include <stdio.h>

#include "driver_backends.h"

void platform_bootstrap(int argc, char ** argv)
{
    (void)argc;
    (void)argv;
    driver_backends_register();
}

int platform_init_io(void)
{
    printf("[platform] 没有可用的显示后端（defconfig 未启用 SDL/FBDEV），以无界面模式运行\n");
    return 0;
}

void platform_shutdown(void)
{
}

const char * platform_name(void)
{
    return "none";
}
