/**
 * @file platform_sdl.c
 * PC 平台引导：SDL2 桌面窗口。
 *
 * 负责命令行解析、后端注册、窗口尺寸与旋转，这些在板子上都不存在，
 * 所以整块放在这里，不再污染 main.c。
 */

#include "platform/platform.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver_backends.h"
#include "simulator_settings.h"
#include "simulator_util.h"

#if LV_USE_SNAPSHOT
#include "lvgl/draw/lv_snapshot.h"
#endif

extern simulator_settings_t settings;

static char * s_selected_backend = NULL;

static void print_usage(void)
{
    fprintf(stdout,
            "\nlvglsim [-V] [-B] [-f] [-m] [-b backend_name] [-W width] [-H height] [-R rotation]\n\n");
    fprintf(stdout, "-V 打印 LVGL 版本\n");
    fprintf(stdout, "-B 列出支持的显示后端\n");
    fprintf(stdout, "-f 全屏\n");
    fprintf(stdout, "-m 最大化\n");
}

static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
}

void platform_bootstrap(int argc, char ** argv)
{
    int opt = 0;

    s_selected_backend = NULL;
    driver_backends_register();

    const char * env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char * env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    settings.window_width  = atoi(env_w ? env_w : "1024");
    settings.window_height = atoi(env_h ? env_h : "600");

    while((opt = getopt(argc, argv, "b:fmW:H:R:BVh")) != -1) {
        switch(opt) {
            case 'h':
                print_usage();
                exit(EXIT_SUCCESS);
            case 'V':
                print_lvgl_version();
                exit(EXIT_SUCCESS);
            case 'B':
                driver_backends_print_supported();
                exit(EXIT_SUCCESS);
            case 'b':
                if(driver_backends_is_supported(optarg) == 0) {
                    die("error no such backend: %s\n", optarg);
                }
                s_selected_backend = strdup(optarg);
                break;
            case 'f':
                settings.fullscreen = true;
                break;
            case 'm':
                settings.maximize = true;
                break;
            case 'W':
                settings.window_width = atoi(optarg);
                break;
            case 'H':
                settings.window_height = atoi(optarg);
                break;
            case 'R':
                switch(atoi(optarg)) {
                    case 0:   settings.rotation = LV_DISPLAY_ROTATION_0;   break;
                    case 90:  settings.rotation = LV_DISPLAY_ROTATION_90;  break;
                    case 180: settings.rotation = LV_DISPLAY_ROTATION_180; break;
                    case 270: settings.rotation = LV_DISPLAY_ROTATION_270; break;
                    default:
                        LV_LOG_WARN("Invalid rotation angle. Valid angles are { 0, 90, 180, 270 }");
                        break;
                }
                break;
            case ':':
                print_usage();
                die("Option -%c requires an argument.\n", optopt);
            case '?':
            default:
                print_usage();
                die("Unknown option -%c.\n", optopt);
        }
    }
}

int platform_init_io(void)
{
    if(s_selected_backend != NULL) {
        if(driver_backends_init_backend(s_selected_backend) == -1) return -1;
    }
    else if(driver_backends_init_backend("SDL") == -1) {
        return -1;
    }

    if(settings.rotation) {
        lv_display_set_rotation(NULL, settings.rotation);
    }
    return 0;
}

void platform_shutdown(void)
{
    /* SDL 后端的资源由 LVGL 与 SDL 自己回收 */
}

const char * platform_name(void)
{
    return "sdl";
}
