/**
 * @file page_face.h
 * 人脸识别页（全屏）：扫描动效 + 实时预览画面。
 *
 * 从主页的「人脸识别」卡片进入。扫描框会根据识别结果变色：
 * 待机是强调色，认出来变绿，没认出来变红（600ms 后自己变回待机）。
 * 颜色由反馈模块（ui_feedback）在收到人脸事件时调这里的函数改。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 扫描框状态。 */
typedef enum {
    PAGE_FACE_SCAN_IDLE = 0,   /* 待机（扫描中） */
    PAGE_FACE_SCAN_OK,         /* 认出来了：绿 */
    PAGE_FACE_SCAN_FAIL,       /* 没认出来：红，600ms 后自动回待机 */
    PAGE_FACE_SCAN_COUNT
} page_face_scan_t;

/* 改扫描框颜色。只改几个控件的颜色，不新建对象、不分配内存。 */
void page_face_set_scan_state(page_face_scan_t st);

/* 读当前扫描框状态（切主题重刷时用）。 */
page_face_scan_t page_face_scan_state(void);

/* 建页面，返回根容器。开机时调一次。 */
lv_obj_t * page_face_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
