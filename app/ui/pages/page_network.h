/**
 * @file page_network.h
 * 网络页：扫描附近热点、显示已保存的网络、输入密码连接。
 *
 * 注意：扫描和连接都要等好几秒，页面里必须放到后台线程做，
 * 不能在界面线程直接调（否则画面会卡住）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_network_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
