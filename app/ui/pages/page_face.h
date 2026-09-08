/**
 * @file page_face.h
 * 人脸识别全屏页：从主页"人脸识别"卡片进入，展示扫描动效 + 占位视频区。
 * 设计要点：极简 / 单一焦点 / 一目了然。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * page_face_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
