/**
 * @file app.h
 * 应用总开关：开机初始化、三个周期任务、退出清理。
 *
 * 调用顺序（全部在主线程）：
 *   1) main() 准备好显示之后调用 app_main()，把各模块按依赖顺序拉起来；
 *   2) 进入主循环后，三个 lv_timer 分别每 20ms / 100ms / 5s 调下面的 tick；
 *   3) 收到退出信号后调用 app_shutdown()。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动：按顺序初始化存储、后台线程、锁、配置、相机、人脸、界面、远程通道。
 * 返回 0 = 成功；非 0 = 启动中止（例如配置检查不通过），由 main 收尾退出。 */
int app_main(void);

/* 每 20ms（主线程）：取后台线程的结果、派发事件、收远程指令、驱动人脸模块。 */
void app_tick_fast(void);

/* 每 100ms（主线程）：推进认证状态机的超时。 */
void app_tick_slow(void);

/* 每 5s（主线程）：向远程通道上报一次设备状态。 */
void app_tick_periodic(void);

/* 退出清理：停采集、关设备、回收线程，只调一次。 */
void app_shutdown(void);

#ifdef __cplusplus
}
#endif
