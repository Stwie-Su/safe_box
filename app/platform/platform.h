/**
 * @file platform.h
 * 平台差异层：把「电脑上的窗口」和「板子上的屏幕」藏在这里。
 *
 * 调用顺序（main.c）：
 *   1) platform_bootstrap() —— lv_init() 之前，解析命令行、注册显示与输入后端；
 *   2) lv_init()；
 *   3) platform_init_io()  —— lv_init() 之后，真正打开屏幕和触摸。
 *
 * 其他层不要写「是 PC 还是板子」的判断，换平台只换本目录下的 .c 文件。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 开机第一步：解析命令行参数，注册所有编进构建的显示与输入后端。 */
void platform_bootstrap(int argc, char ** argv);

/* 显示与输入初始化。返回 0 成功；失败时 main 会直接退出。 */
int platform_init_io(void);

/* 退出清理。 */
void platform_shutdown(void);

/* 平台名："sdl" / "fbdev" / "none"，只用于日志和状态上报。 */
const char * platform_name(void);

#ifdef __cplusplus
}
#endif
