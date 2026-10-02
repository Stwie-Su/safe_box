/**
 * @file fbdev.c
 *
 * Legacy framebuffer device
 *
 * Based on the original file from the repository
 *
 * Move to a separate file
 * 2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lvgl/lvgl.h"
#if LV_USE_LINUX_FBDEV
#include "../simulator_util.h"
#include "../backends.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static lv_display_t * init_fbdev(void);

/**********************
 *  STATIC VARIABLES
 **********************/

static char * backend_name = "FBDEV";

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Register the backend
 *
 * @param backend the backend descriptor
 * @description configures the descriptor
 */
int backend_init_fbdev(backend_t * backend)
{
    LV_ASSERT_NULL(backend);

    backend->handle->display = malloc(sizeof(display_backend_t));
    LV_ASSERT_NULL(backend->handle->display);

    backend->handle->display->init_display = init_fbdev;
    backend->name = backend_name;
    backend->type = BACKEND_DISPLAY;

    return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initialize the fbdev driver
 *
 * @return the LVGL display
 */
static lv_display_t * init_fbdev(void)
{
    const char * device = getenv_default("LV_LINUX_FBDEV_DEVICE", "/dev/fb0");
    lv_display_t * disp = lv_linux_fbdev_create();

    if(disp == NULL) {
        return NULL;
    }

    lv_linux_fbdev_set_file(disp, device);

    /* 画面旋转：SAFE_DISPLAY_ROT=180 时整屏软件旋转 180 度（板端屏幕倒装场景）。
     * 渲染侧由本后端 flush 阶段的 lv_draw_sw_rotate 完成；触摸侧由 evdev 后端
     * 按同一环境变量联动做双轴镜像（见 evdev.c evdev_apply_calibration）。 */
    {
        const char * rot_env = getenv("SAFE_DISPLAY_ROT");
        if(rot_env != NULL && strcmp(rot_env, "180") == 0) {
            lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
            LV_LOG_USER("fbdev: SAFE_DISPLAY_ROT=180 -> display rotated 180");
        }
    }

    return disp;
}

#endif /*LV_USE_LINUX_FBDEV*/
