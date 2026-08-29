#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

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
#include "ui/theme.h"
#if LV_USE_SNAPSHOT
#include "lvgl/draw/lv_snapshot.h"
#endif
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
    settings.window_width  = atoi(env_w ? env_w : (LV_USE_SDL ? "1843" : "1024"));
    settings.window_height = atoi(env_h ? env_h : (LV_USE_SDL ? "1080" : "600"));

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


/* ============================================================
 * 开发板：防息屏 + 触摸唤醒
 * ---------------------------------------------------------------------------
 * 现象：上电后过几分钟屏幕自己黑掉，再点屏幕也点不亮。
 *
 * 根因：内核 VT(fbcon) 的 console blanking。本板 /sys/module/kernel/parameters/
 * consoleblank = 600（10 分钟），到点后 fbcon 会对 /dev/fb0 下发 FB_BLANK_POWERDOWN。
 * 这条链路完全发生在内核里，应用层毫无感知：
 *   - LVGL 走 FBDEV 后端直接写 framebuffer，上层照常刷新，但面板已关 -> 黑屏；
 *   - 触摸事件走 /dev/input/event1（evdev），与 VT 层无关，LVGL 收得到点击，
 *     但没有任何人去点亮面板 -> “点了没反应”。
 * 且 consoleblank 是只读内核参数（S_IRUGO），运行时 echo 不进去（Permission denied），
 * 改 bootargs 又要动 U-Boot，所以最稳妥是在应用层解决。
 *
 * 双保险修法：
 *   1) KDSETMODE KD_GRAPHICS：声明该 VT 归图形程序所有，从源头跳过 VT blank 逻辑
 *      （X11 / Qt eglfs 接管控制台也是这一步）。
 *   2) 200ms 轮询兜底：一旦检测到“刚刚发生过输入”，立刻 FBIOBLANK UNBLANK。
 *      息屏后触摸事件照样能到 LVGL，inactive_time 会归零，
 *      于是下一拍（<=200ms）必定把屏幕点亮，首次触摸即可唤醒。
 * ============================================================ */
#if LV_USE_LINUX_FBDEV
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>

#define KEEPALIVE_POLL_MS   200     /* 轮询周期，决定唤醒延迟上限 */
#define KEEPALIVE_FRESH_MS  400     /* 多久内有输入算“刚活动过” */

static int s_fb_keepalive_fd = -1;

static void fb_keepalive_init(void)
{
    /* 1) 抢占 VT，从源头禁掉 console blanking */
    int tty = open("/dev/tty1", O_RDWR);
    if(tty >= 0) {
        if(ioctl(tty, KDSETMODE, KD_GRAPHICS) == 0) {
            LV_LOG_USER("keepalive: /dev/tty1 -> KD_GRAPHICS (VT blanking disabled)");
        }
        else {
            LV_LOG_USER("keepalive: KDSETMODE failed: %s", strerror(errno));
        }
        close(tty);
    }
    else {
        LV_LOG_USER("keepalive: cannot open /dev/tty1: %s", strerror(errno));
    }

    /* 2) 留一份 fb0 句柄，必要时强制点亮 */
    s_fb_keepalive_fd = open("/dev/fb0", O_RDWR);
    if(s_fb_keepalive_fd < 0) {
        LV_LOG_USER("keepalive: cannot open /dev/fb0: %s", strerror(errno));
    }
}

static void fb_keepalive_cb(lv_timer_t * t)
{
    (void)t;
    if(s_fb_keepalive_fd < 0) return;

    /* 只有“刚刚发生过输入”才下发 ioctl，避免每拍都打驱动 */
    if(lv_display_get_inactive_time(NULL) > KEEPALIVE_FRESH_MS) return;

    ioctl(s_fb_keepalive_fd, FBIOBLANK, FB_BLANK_UNBLANK);
}
#endif /* LV_USE_LINUX_FBDEV */

/* ============================================================
 * 性能探针（现场排查卡顿用，SAFE_PERF_LOG=1 打开，默认关闭、零开销）
 * ---------------------------------------------------------------------------
 * 为什么不用 LV_USE_PERF_MONITOR：它会在屏幕右下角画一个 FPS 覆盖层，
 * 本身要渲染、还污染画面，不适合量产；而且它只给 FPS，看不出"抖动"。
 * 这里直接在主循环里测 lv_timer_handler() 的单次耗时并打到日志：
 *   avg  -> 平均每次刷新循环耗时（稳态开销）
 *   max  -> 2 秒内的最大单次耗时（卡顿尖峰，最能反映"点下去要等一下"）
 * 用 LV_LOG_USER 输出，默认日志级别(WARN)即可见，不需要开 TRACE
 * （实测 TRACE 级日志约 1MB/s，在单核 Cortex-A7 上本身就是卡顿源）。
 * ============================================================ */
static bool     s_perf_on     = false;
static uint32_t s_perf_loops  = 0;
static uint32_t s_perf_ms_sum = 0;
static uint32_t s_perf_ms_max = 0;

static void perf_log_cb(lv_timer_t * t)
{
    (void)t;
    if(s_perf_loops == 0) return;
    LV_LOG_USER("perf: cycles=%u  handler avg=%u ms  max=%u ms",
                (unsigned)s_perf_loops,
                (unsigned)(s_perf_ms_sum / s_perf_loops),
                (unsigned)s_perf_ms_max);
    s_perf_loops = 0;
    s_perf_ms_sum = 0;
    s_perf_ms_max = 0;
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

#if LV_USE_LINUX_FBDEV
    /* 开发板：接管 VT + 挂上防息屏/唤醒定时器（必须在 UI 创建前，避免首帧就被息屏） */
    fb_keepalive_init();
    lv_timer_create(fb_keepalive_cb, KEEPALIVE_POLL_MS, NULL);
#endif

    /* 性能探针：SAFE_PERF_LOG=1 打开，每 2 秒打一行循环耗时 */
    {
        const char * pe = getenv("SAFE_PERF_LOG");
        if(pe && *pe && strcmp(pe, "0") != 0) {
            s_perf_on = true;
            lv_timer_create(perf_log_cb, 2000, NULL);
        }
    }

    // 启动业务应用：初始化开锁执行器 + 创建整套 UI
    actuator_init();

    /* SAFE_DIR 环境变量可覆盖数据目录（PC 调试用） */
    {
        const char *sd = getenv("SAFE_DIR");
        if (sd && *sd) store_set_dir(sd);
    }

    app_start();

    /* ============================================================
     * 测试模式：环境变量控制起始页 / 起始主题（仅 PC 调试用）
     *   SAFE_TEST_PAGE   = HOME | LOGS | SETTINGS | USERS | NETWORK | SYSTEM | KEYPAD
     *   SAFE_TEST_THEME  = 0..3   (0=石墨黑 1=月白 2=蓝白 3=松石青)
     *   SAFE_TEST_DLG    = add_user  在 USERS 页加载后自动打开"添加用户"弹窗
     *   SAFE_TEST_SHOT   = /tmp/x.png  非空时初始化完截一张图
     * ============================================================ */
    {
        const char * td = getenv("SAFE_TEST_DLG");
        if (td && *td && (!strcmp(td, "add_user") || !strcmp(td, "auth") || !strcmp(td, "change_pwd"))) {
            ui_switch_page(PAGE_USERS);
            extern void page_users_test_open_add_dlg(void);
            extern void page_users_test_open_auth_dlg(void);
            extern void page_users_test_open_change_pwd_dlg(void);
            if (!strcmp(td, "add_user"))      page_users_test_open_add_dlg();
            else if (!strcmp(td, "auth"))     page_users_test_open_auth_dlg();
            else                              page_users_test_open_change_pwd_dlg();
        }
        const char * tp = getenv("SAFE_TEST_PAGE");
        const char * tt = getenv("SAFE_TEST_THEME");
        if (tt && *tt) theme_switch(atoi(tt));
        if (tp && *tp) {
            ui_page_t pid = PAGE_HOME;
            if (!strcmp(tp, "LOGS")) pid = PAGE_LOGS;
            else if (!strcmp(tp, "SETTINGS")) pid = PAGE_SETTINGS;
            else if (!strcmp(tp, "USERS")) pid = PAGE_USERS;
            else if (!strcmp(tp, "NETWORK")) pid = PAGE_NETWORK;
            else if (!strcmp(tp, "SYSTEM")) pid = PAGE_SYSTEM;
            else if (!strcmp(tp, "KEYPAD")) pid = PAGE_KEYPAD;
            ui_switch_page(pid);
        }
        const char * sh = getenv("SAFE_TEST_SHOT");
        if (sh && *sh) {
            /* 跑 5 帧让布局/字体稳定再截 */
            for (int i = 0; i < 5; i++) lv_timer_handler();
            lv_draw_buf_t * s = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
            if (s) {
                /* lvgl 9.2 snapshot 返回 lv_draw_buf_t; data/size 在结构体里 */
                FILE * fp = fopen(sh, "wb");
                if (fp) {
                    uint32_t row_bytes = s->header.stride ? s->header.stride
                                       : s->header.w * lv_color_format_get_bpp(s->header.cf) / 8;
                    uint8_t * p8 = (uint8_t *)s->data;
                    for (uint32_t y = 0; y < s->header.h; y++) {
                        fwrite(p8 + y * row_bytes, 1, s->header.w * 2, fp);
                    }
                    fclose(fp);
                }
                lv_draw_buf_destroy(s);
            }
            fflush(stdout); fflush(stderr);
            _exit(0);
        }
    }

    // 主循环：不断处理 LVGL 定时器与界面刷新
    while(1) {
        uint32_t t0 = s_perf_on ? lv_tick_get() : 0;
        uint32_t ms = lv_timer_handler();//距离下一次任务还有多久
        if(s_perf_on) {
            uint32_t dt = lv_tick_get() - t0;
            s_perf_loops++;
            s_perf_ms_sum += dt;
            if(dt > s_perf_ms_max) s_perf_ms_max = dt;
        }
        if(ms == LV_NO_TIMER_READY) { // 此时如果没有任务，ms 等于 0xFFFFFFFF
            ms = LV_DEF_REFR_PERIOD;
        }
        usleep(ms * 1000);
    }

    return 0;
}








