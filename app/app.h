/**
 * @file app.h
 * 应用编排：初始化顺序、周期任务节拍、退出清理。
 *
 * 本模块不依赖 LVGL，周期任务由 main.c 用 lv_timer 挂在 LVGL 时钟上，
 * 这样单元测试也能直接调用这些节拍函数。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动：存储、工作线程、执行器、配置、界面、状态机、远程通道，顺序有依赖。 */
void app_main(void);

/* 20ms：异步结果泵、事件总线派发、指令泵、人脸服务心跳 */
void app_tick_fast(void);

/* 100ms：认证状态机心跳（状态超时推进） */
void app_tick_slow(void);

/* 5s：状态周期上报 */
void app_tick_periodic(void);

/* 退出清理 */
void app_shutdown(void);

#ifdef __cplusplus
}
#endif
