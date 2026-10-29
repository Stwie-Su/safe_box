/**
 * @file page_enroll.h
 * 人脸录入页（全屏）：专门用来录人脸，和「人脸识别」页分开。
 *
 * 进入：用户页点「录入人脸」→ 切到本页；
 * 退出：录完、失败、或 30 秒超时自动回用户页，也可以点「取消录入」。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_enroll_create(lv_obj_t * parent);

/* 本页现在是不是显示中。人脸线程用这个判断要不要开相机和模组
 * （识别页和录入页共用同一套硬件，两个页面任一个在显示就要工作）。 */
bool page_enroll_is_visible(void);

/* 在本页显示录入结果。
 *   text 传 NULL = 清掉结果，恢复成正常状态；
 *   ok = true 用成功色，false 用警告色（按钮会变成「重新录入」）。
 * 由用户页在收到「录入完成」事件后调。 */
void page_enroll_show_result(const char * text, bool ok);

#ifdef __cplusplus
}
#endif
