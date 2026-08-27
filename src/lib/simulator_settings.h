/**
 * @file simulator_settings.h
 *
 * global simulator settings
 *
 * The simulator settings is a global variable defined in
 * simulator_settings.c
 *
 * Copyright (c) 2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 */

#ifndef SIMULATOR_SETTINGS_H
#define SIMULATOR_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl/lvgl.h"

/*********************
 *      DEFINES
 *********************/

/* PC 验证窗口默认尺寸 = 目标硬件分辨率（i.MX6ULL LCD 1024×600）。
 * 与 DESIGN.md 的界面基准保持一致，否则内容区高度不足导致页面溢出/裁切。 */
#define LV_SIM_DEFAULT_WIDTH     1024
#define LV_SIM_DEFAULT_HEIGHT    600
/* 字符串版，供 main.c 的 atoi() 默认值使用 */
#define LV_SIM_DEFAULT_WIDTH_STR "1024"
#define LV_SIM_DEFAULT_HEIGHT_STR "600"

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    uint32_t window_width;
    uint32_t window_height;
    lv_display_rotation_t rotation;
    bool maximize;
    bool fullscreen;
} simulator_settings_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SIMULATOR_SETTINGS_H*/
