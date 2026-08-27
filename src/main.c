#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

/* 是否编译内置 demo（由 defconfig 的 LV_BUILD_DEMOS 决定） */
#if LV_BUILD_DEMOS
    #include <lv_demos.h>
#endif

/* 是否编译示例（由 LV_BUILD_EXAMPLES 决定） */
#if LV_BUILD_EXAMPLES
    #include <lv_examples.h>
#endif

/* 卡车 demo 开关（CONFIG_ 前缀来自 Kconfig 生成的头文件） */
#ifdef CONFIG_LV_USE_DEMO_TRUCK
    #include <lv_demo_truck.h>
#endif

/* 后端框架头文件：提供 register / init / is_supported 等函数 */
#include "lib/driver_backends.h"
#include "lib/simulator_util.h"
#include "lib/simulator_settings.h"

/* 业务 UI 与硬件执行器（开锁机构） */
#include "ui/ui.h"
#include "hal/actuator.h"
#include "core/store.h"

/* ===================== 内部函数声明 ===================== */
static void __attribute__((unused)) configure_simulator(int argc, char ** argv);  /* PC 专用：解析命令行并注册后端 */
static void print_lvgl_version(void);                     /* 打印 LVGL 版本 */
static void print_usage(void);                            /* 打印帮助信息 */

/* 用户在命令行用 -b 指定的后端名；未指定时为 NULL */
static char * selected_backend;

/* 仿真器全局设置（窗口尺寸、是否全屏、旋转角度等），定义在其他文件 */
extern simulator_settings_t settings;

/* 打印 LVGL 版本号 */
static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LVGL_VERSION_INFO);
}

/* 打印命令行用法 */
static void print_usage(void)
{
    fprintf(stdout,
            "\nlvglsim [-V] [-B] [-f] [-m] [-b backend_name] [-W window_width] [-H window_height] [-R rotation]\n\n");
    fprintf(stdout, "-V 打印 LVGL 版本\n");
    fprintf(stdout, "-B 列出支持的显示后端\n");
    fprintf(stdout, "-f 全屏\n");
    fprintf(stdout, "-m 最大化\n");
}

/* ============================================================
 * PC 专用：解析命令行参数并注册所有显示/输入后端
 * 仅当 LV_USE_SDL（PC 平台）时才会被调用。
 * 内部关键步骤：
 *   1) driver_backends_register() —— 把编译进来的后端登记到列表
 *   2) 读环境变量设置默认窗口尺寸
 *   3) getopt 解析 -b/-W/-H/-R 等参数
 * 注意：开发板不调用本函数（见 main 中的宏判断），改走 driver_backends_register()。
 * ============================================================ */
static void __attribute__((unused)) configure_simulator(int argc, char ** argv)
{
    int opt = 0;

    selected_backend = NULL;          // 先清空“用户指定后端”
    driver_backends_register();       // 注册后端（SDL 等），必须执行

    // 从环境变量取窗口尺寸，没有就用目标硬件默认（1024×600，见 simulator_settings.h）
    const char * env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char * env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    settings.window_width  = atoi(env_w ? env_w : LV_SIM_DEFAULT_WIDTH_STR);
    settings.window_height = atoi(env_h ? env_h : LV_SIM_DEFAULT_HEIGHT_STR);

    // 解析命令行参数
    while((opt = getopt(argc, argv, "b:fmW:H:R:BVh")) != -1) {
        switch(opt) {
            case 'h':                 // 帮助
                print_usage();
                exit(EXIT_SUCCESS);
            case 'V':                 // 版本
                print_lvgl_version();
                exit(EXIT_SUCCESS);
            case 'B':                 // 列出后端
                driver_backends_print_supported();
                exit(EXIT_SUCCESS);
            case 'b':                 // 指定后端，例如 -b SDL 或 -b FBDEV
                if(driver_backends_is_supported(optarg) == 0) {
                    die("error no such backend: %s\n", optarg);
                }
                selected_backend = strdup(optarg);
                break;
            case 'f':                 // 全屏
                settings.fullscreen = true;
                break;
            case 'm':                 // 最大化
                settings.maximize = true;
                break;
            case 'W':                 // 窗口宽
                settings.window_width = atoi(optarg);
                break;
            case 'H':                 // 窗口高
                settings.window_height = atoi(optarg);
                break;
            case 'R':                 // 旋转角度 0/90/180/270
                switch(atoi(optarg)) {
                    case 0:   settings.rotation = LV_DISPLAY_ROTATION_0;   break;
                    case 90:  settings.rotation = LV_DISPLAY_ROTATION_90;  break;
                    case 180: settings.rotation = LV_DISPLAY_ROTATION_180; break;
                    case 270: settings.rotation = LV_DISPLAY_ROTATION_270; break;
                    default:
                        LV_LOG_WARN("Invalid rotation angle. Valid angles are {0, 90, 180, 270}");
                        break;
                }
                break;
            case ':':                 // 缺参数的选项
                print_usage();
                die("Option -%c requires an argument.\n", optopt);
            case '?':                 // 未知选项
                print_usage();
                die("Unknown option -%c.\n", optopt);
        }
    }
}


int main(int argc, char ** argv)
{
    // 根据平台宏决定如何初始化后端：
    //   PC（LV_USE_SDL）：调用 configure_simulator 解析命令行并注册后端
    //   开发板（否则）：  跳过命令行解析，但必须保留 driver_backends_register()，
    //                    否则后端列表为空，下面 init_backend("FBDEV") 会失败
#if LV_USE_SDL
    configure_simulator(argc, argv);
#else
    driver_backends_register();
#endif

    lv_init();   // 初始化 LVGL 核心

    /* ============================================================
     * 显示后端初始化：Ubuntu(PC) vs 开发板 对照
     * 编译进哪些后端由 defconfig 决定（pc.defconfig→sdl.c；
     * get_started.defconfig→fbdev.c）。本文件用 #if 自动选择，
     * 无需手动改：PC 走 SDL 分支，板子走 FBDEV 分支。
     * 命令行 -b <name> 可强制指定后端，覆盖默认选择。
     * ============================================================ */
    bool disp_inited = false;

    if(selected_backend != NULL) {
        // 用户显式指定了后端：./lvglsim -b SDL | -b FBDEV
        if(driver_backends_init_backend(selected_backend) == -1) {
            die("Failed to initialize display backend: %s", selected_backend);
        }
        disp_inited = true;
    }
#if LV_USE_SDL // PC：初始化 SDL 显示后端（弹桌面窗口）
    else {
        if(driver_backends_init_backend("SDL") == -1) {
            die("Failed to initialize SDL display backend");
        }
        disp_inited = true;
    }
#elif LV_USE_LINUX_FBDEV // 开发板：初始化 FBDEV 显示后端（直接写 /dev/fb0）
    else {
        if(driver_backends_init_backend("FBDEV") == -1) {
            die("Failed to initialize FBDEV display backend");
        }
        disp_inited = true;
    }
#endif
    if(!disp_inited) {
        // 兜底：用默认注册的后端（仅当无平台后端分支可用时才走到这里）
        if(driver_backends_init_backend(NULL) == -1) {
            die("Failed to initialize display backend");
        }
    }

    // 若设置了旋转角度，应用到显示器
    if(settings.rotation) {
#if LV_USE_DRAW_NANOVG && LV_DRAW_TRANSFORM_USE_MATRIX
        lv_display_set_matrix_rotation(NULL, true);
#endif
        lv_display_set_rotation(NULL, settings.rotation);
    }

    // 触摸输入：仅开发板生效（PC 上 LV_USE_EVDEV 未定义，本段被编译剔除）
#if LV_USE_EVDEV
    if(driver_backends_init_backend("EVDEV") == -1) {
        die("Failed to initialize evdev");
    }
#endif

    // 启动业务应用：初始化开锁执行器 + 创建整套 UI
    actuator_init();

    /* SAFE_DIR 环境变量可覆盖数据目录（PC 调试用） */
    {
        const char *sd = getenv("SAFE_DIR");
        if (sd && *sd) store_set_dir(sd);
    }

    app_start();

    // 主循环：不断处理 LVGL 定时器与界面刷新
    while(1) {
        uint32_t ms = lv_timer_handler();//距离下一次任务还有多久
        if(ms == LV_NO_TIMER_READY) { // 此时如果没有任务，ms 等于 0xFFFFFFFF
            ms = LV_DEF_REFR_PERIOD;
        }
        usleep(ms * 1000);
    }

    return 0;
}
