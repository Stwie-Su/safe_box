/**
 * @file platform.h
 * 平台引导接口：把「PC 桌面窗口」与「开发板帧缓冲」的差异收在这里。
 *
 * 调用顺序（main.c）：
 *   1) platform_bootstrap(argc, argv)  —— lv_init() 之前：解析命令行、注册后端
 *   2) lv_init()
 *   3) platform_init_io()              —— lv_init() 之后：初始化显示与输入
 *
 * 其余各层不出现平台判断，换平台只换 platform 目录下各 .c 文件的编译选择（由 defconfig 决定）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* lv_init 之前：解析命令行参数并注册所有已编译的显示与输入后端。 */
void platform_bootstrap(int argc, char ** argv);

/* lv_init 之后：初始化显示与输入设备。返回 0 成功。 */
int platform_init_io(void);

/* 退出清理。 */
void platform_shutdown(void);

/* 平台名："sdl" / "fbdev" / "none"，用于日志与状态上报。 */
const char * platform_name(void);

#ifdef __cplusplus
}
#endif
