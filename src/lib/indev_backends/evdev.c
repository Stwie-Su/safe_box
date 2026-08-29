/**
 *
 * @file evdev.c
 *
 * The lib evdev driver
 *
 * Based on the original file from the repository
 *
 * - Move the driver to a separate file to avoid excessive conditional
 *   compilation
 *   Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 * Copyright (c) 2025 EDGEMTech Ltd.
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "lvgl/lvgl.h"
#if LV_USE_EVDEV
#include <lvgl_private/lvgl_private.h>
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

static void indev_deleted_cb(lv_event_t * e);
static void discovery_cb(lv_indev_t * indev, lv_evdev_type_t type, void * user_data);
static void set_mouse_cursor_icon(lv_indev_t * indev, lv_display_t * display);
static lv_indev_t * init_pointer_evdev(lv_display_t * display);

/**********************
 *  STATIC VARIABLES
 **********************/

static char * backend_name = "EVDEV";

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*
 * Initialize the evdev driver
 *
 * @param backend the backend descriptor
 */
int backend_init_evdev(backend_t * backend)
{
    LV_ASSERT_NULL(backend);
    backend->handle->indev = malloc(sizeof(indev_backend_t));
    LV_ASSERT_NULL(backend->handle->indev);

    backend->handle->indev->init_indev = init_pointer_evdev;

    backend->name = backend_name;
    backend->type = BACKEND_INDEV;
    return 0;
}


/**********************
 *   STATIC FUNCTIONS
 **********************/

/*
 * Remove cursor icon
 *
 * @description When the indev is deleted remove the mouse cursor icon
 * @note called by the LVGL evdev driver
 * @param e the deletion event
 */
static void indev_deleted_cb(lv_event_t * e)
{
    if(LV_GLOBAL_DEFAULT()->deinit_in_progress) return;
    lv_obj_t * cursor_obj = lv_event_get_user_data(e);
    lv_obj_delete(cursor_obj);
}


/*
 * Set cursor icon
 *
 * @description Once the input device is discovered set the mouse cursor icon
 * @note called by the LVGL evdev driver
 * @param indev the input device
 * @param type the type of the input device
 * @param user_data the user data
 */
static void discovery_cb(lv_indev_t * indev, lv_evdev_type_t type, void * user_data)
{
    LV_LOG_USER("new '%s' device discovered", type == LV_EVDEV_TYPE_REL ? "REL" :
                type == LV_EVDEV_TYPE_ABS ? "ABS" :
                type == LV_EVDEV_TYPE_KEY ? "KEY" :
                "unknown");

    lv_display_t * disp = user_data;
    lv_indev_set_display(indev, disp);

    if(type == LV_EVDEV_TYPE_REL) {
        set_mouse_cursor_icon(indev, disp);
    }
}

/*
 * Set cursor icon
 *
 * @description Enables a pointer (touchscreen/mouse) input device
 * @param display the display on which to create
 * @param indev the input device to set the cursor on
 */
static void set_mouse_cursor_icon(lv_indev_t * indev, lv_display_t * display)
{
    /* Set the cursor icon */
    LV_IMAGE_DECLARE(mouse_cursor_icon);
    lv_obj_t * cursor_obj = lv_image_create(lv_display_get_screen_active(display));
    lv_image_set_src(cursor_obj, &mouse_cursor_icon);
    lv_indev_set_cursor(indev, cursor_obj);

    /* delete the mouse cursor icon if the device is removed */
    lv_indev_add_event_cb(indev, indev_deleted_cb, LV_EVENT_DELETE, cursor_obj);

}

/*
 * Initialize a mouse pointer device
 *
 * Enables a pointer (touchscreen/mouse) input device
 * Use 'evtest' to find the correct input device. /dev/input/by-id/ is recommended if possible
 * Use /dev/input/by-id/my-mouse-or-touchscreen or /dev/input/eventX
 *
 * If LV_LINUX_EVDEV_POINTER_DEVICE is not set, automatic evdev discovery will start
 *
 * @param display the LVGL display
 *
 * @return input device
 */

/*
 * ---------------------------------------------------------------------------
 *  触摸屏标定（i.MX6ULL + Goodix-TS，2026-08-29 用 266 次真实手指点击定标）
 * ---------------------------------------------------------------------------
 * 驱动声明的能力（板子上 EVIOCGABS 实测打印）：
 *     ABS_X  = 0..0        ABS_Y = 0..0          <- 单点协议，驱动根本不用
 *     ABS_MT_POSITION_X = 0..800   ABS_MT_POSITION_Y = 0..480
 *
 * 但这两组“驱动声明值”都【不可信】：实测手指点击上报的原始坐标
 * X 最大 912、Y 最大 568 —— 说明驱动上报的本来就是 1024x600 的 LCD 像素坐标，
 * 声明的 800x480 只是没适配本屏的固件默认值。
 *
 * 曾经的错误做法：把声明量程 0..800 / 0..480 交给 lv_evdev_set_calibration()。
 * LVGL 会按  (raw - in_min) * (out_max - out_min) / (in_max - in_min) + out_min
 * 做线性映射，等效把每次点击放大 1024/800 = 1.28 倍：
 *     实测 (407,268) 本该命中密码盘 "4"，放大成 (521,335) 后命中 "8"，
 *     表现为“点这个键、触发那个键”的整体错位。
 *
 * 正确做法：默认按“驱动上报值即屏幕像素”处理 —— 把标定范围设成
 * (0, 0, hor_res-1, ver_res-1)，映射恒等，顺带把出界点钳到屏内。
 * 现场若真遇到需要缩放的面板，用 LV_TOUCH_CAL_* 环境变量覆盖，不改代码。
 * ---------------------------------------------------------------------------
 */

/*
 * 读取设备声明的 ABS 量程（多点协议 B 优先，退化到单点协议）。
 * 现在只用于【诊断打印】和判断“是不是绝对坐标设备”，不再直接拿来做标定。
 * @return 1 = 读到有效绝对量程（触摸屏），0 = 读不到（多半是鼠标）
 */
static int evdev_probe_abs_range(const char * dev_path,
                                 struct input_absinfo * xinfo,
                                 struct input_absinfo * yinfo)
{
    if(dev_path == NULL) return 0;

    int fd = open(dev_path, O_RDONLY);
    if(fd < 0) {
        LV_LOG_USER("evdev: cannot open %s for probing: %s", dev_path, strerror(errno));
        return 0;
    }

    int ok = 0;
    /* 电容屏普遍走多点协议 B，优先用它的量程 */
    if(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), xinfo) == 0 &&
       ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), yinfo) == 0 &&
       xinfo->maximum > xinfo->minimum && yinfo->maximum > yinfo->minimum) {
        ok = 1;
    }
    /* 退化到单点协议（电阻屏 / 老驱动） */
    else if(ioctl(fd, EVIOCGABS(ABS_X), xinfo) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_Y), yinfo) == 0 &&
            xinfo->maximum > xinfo->minimum && yinfo->maximum > yinfo->minimum) {
        ok = 1;
    }

    close(fd);
    return ok;
}

/*
 * 应用标定：默认恒等（上报值 == 屏幕像素），支持现场环境变量覆盖。
 *
 * 环境变量（可选，单位都是“驱动上报的原始值”）：
 *   LV_TOUCH_CAL_MINX / LV_TOUCH_CAL_MINY / LV_TOUCH_CAL_MAXX / LV_TOUCH_CAL_MAXY
 */
static void evdev_apply_calibration(lv_indev_t * indev, lv_display_t * display)
{
    const int32_t w = (int32_t)lv_display_get_horizontal_resolution(display);
    const int32_t h = (int32_t)lv_display_get_vertical_resolution(display);

    int32_t minx = 0, miny = 0, maxx = w - 1, maxy = h - 1;

    const char * e;
    if((e = getenv("LV_TOUCH_CAL_MINX")) != NULL && *e) minx = atoi(e);
    if((e = getenv("LV_TOUCH_CAL_MINY")) != NULL && *e) miny = atoi(e);
    if((e = getenv("LV_TOUCH_CAL_MAXX")) != NULL && *e) maxx = atoi(e);
    if((e = getenv("LV_TOUCH_CAL_MAXY")) != NULL && *e) maxy = atoi(e);

    lv_evdev_set_calibration(indev, minx, miny, maxx, maxy);
    LV_LOG_USER("evdev: calibration (%d,%d)-(%d,%d) -> display %dx%d",
                (int)minx, (int)miny, (int)maxx, (int)maxy, (int)w, (int)h);
}

/* 诊断：打印驱动自己声明的量程，便于现场核对“声明值”与“实际上报值”是否一致 */
static void evdev_dump_declared_range(const char * dev_path)
{
    struct input_absinfo xi, yi;
    if(!evdev_probe_abs_range(dev_path, &xi, &yi)) {
        LV_LOG_USER("evdev: %s declares no usable ABS range", dev_path);
        return;
    }
    LV_LOG_USER("evdev: %s DECLARED X %d..%d  Y %d..%d"
                " (informational only, NOT used for calibration)",
                dev_path, xi.minimum, xi.maximum, yi.minimum, yi.maximum);
}

static lv_indev_t * init_pointer_evdev(lv_display_t * display)
{
    const char * input_device = getenv("LV_LINUX_EVDEV_POINTER_DEVICE");

    if(input_device == NULL) {
        LV_LOG_USER("Using evdev automatic discovery.");
        lv_evdev_discovery_start(discovery_cb, display);
        return NULL;
    }

    lv_indev_t * indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_device);

    if(indev == NULL) {
        return NULL;
    }

    lv_indev_set_display(indev, display);

    /* 先打印驱动声明量程（仅诊断），再按“上报值即像素”标定 */
    evdev_dump_declared_range(input_device);
    evdev_apply_calibration(indev, display);

    /* 能读到绝对量程 = 触摸屏，不画鼠标箭头；读不到 = 鼠标/相对设备，才画光标 */
    struct input_absinfo xi, yi;
    if(!evdev_probe_abs_range(input_device, &xi, &yi)) {
        set_mouse_cursor_icon(indev, display);
    }

    return indev;
}
#endif /*#if LV_USE_EVDEV*/

