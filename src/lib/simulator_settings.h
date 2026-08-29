/**
 * @file simulator_settings.h
 *
 * global simulator settings
 *
 * Copyright (c) 2025 EDGEMTech Ltd.
 */

#ifndef SIMULATOR_SETTINGS_H
#define SIMULATOR_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/*
 * Window default size:
 *   PC (SDL): 1.8x target HW resolution for easier desktop debugging (1843x1080).
 *   Board (FBDEV): reads physical resolution from /dev/fb0 ioctl, this macro unused.
 */
#ifdef LV_USE_SDL
  #define LV_SIM_DEFAULT_WIDTH     1843
  #define LV_SIM_DEFAULT_HEIGHT    1080
#else
  #define LV_SIM_DEFAULT_WIDTH     1024
  #define LV_SIM_DEFAULT_HEIGHT    600
#endif
#define LV_SIM_DEFAULT_WIDTH_STR  STRINGIFY(LV_SIM_DEFAULT_WIDTH)
#define LV_SIM_DEFAULT_HEIGHT_STR STRINGIFY(LV_SIM_DEFAULT_HEIGHT)

#ifndef STRINGIFY
#  define STRINGIFY(x) #x
#endif

typedef struct {
    uint32_t window_width;
    uint32_t window_height;
    lv_display_rotation_t rotation;
    bool maximize;
    bool fullscreen;
} simulator_settings_t;

#ifdef __cplusplus
}
#endif

#endif /*SIMULATOR_SETTINGS_H*/
