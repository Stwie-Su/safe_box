/**
 * @file platform_fbdev.c
 * 开发板平台引导：帧缓冲显示 + evdev 触摸，附带防息屏保活。
 *
 * 防息屏这一段是从旧 main.c 搬过来的现场经验，别删：
 * 内核 VT 的 console blanking（本板 consoleblank=600）会在应用层毫无感知的情况下
 * 对 fb0 下发 FB_BLANK_POWERDOWN，表现是"过几分钟黑屏，点屏幕也点不亮"。
 * consoleblank 是只读内核参数，改 bootargs 要动 U-Boot，所以在应用层做双保险：
 *   1) KDSETMODE KD_GRAPHICS 声明该 VT 归图形程序所有，从源头跳过 VT blank；
 *   2) 200ms 轮询兜底，检测到刚有输入就 FBIOBLANK UNBLANK，首次触摸即可唤醒。
 */

#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "driver_backends.h"

#include "lvgl.h"

#define KEEPALIVE_POLL_MS    200
#define KEEPALIVE_FRESH_MS   400

static int s_fb_keepalive_fd = -1;

static void fb_keepalive_init(void)
{
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

    s_fb_keepalive_fd = open("/dev/fb0", O_RDWR);
    if(s_fb_keepalive_fd < 0) {
        LV_LOG_USER("keepalive: cannot open /dev/fb0: %s", strerror(errno));
    }
}

static void fb_keepalive_cb(lv_timer_t * t)
{
    (void)t;
    if(s_fb_keepalive_fd < 0) return;

    /* 只有刚发生过输入才下发 ioctl，避免每拍都打驱动 */
    if(lv_display_get_inactive_time(NULL) > KEEPALIVE_FRESH_MS) return;

    ioctl(s_fb_keepalive_fd, FBIOBLANK, FB_BLANK_UNBLANK);
}

void platform_bootstrap(int argc, char ** argv)
{
    (void)argc;
    (void)argv;
    /* 板子不解析命令行，但后端列表必须注册，否则 init_backend("FBDEV") 会失败 */
    driver_backends_register();
}

int platform_init_io(void)
{
    if(driver_backends_init_backend("FBDEV") == -1) return -1;

#if LV_USE_EVDEV
    if(driver_backends_init_backend("EVDEV") == -1) return -1;
#endif

    /* 必须在 UI 创建前接管 VT，避免首帧就被息屏 */
    fb_keepalive_init();
    lv_timer_create(fb_keepalive_cb, KEEPALIVE_POLL_MS, NULL);
    return 0;
}

void platform_shutdown(void)
{
    if(s_fb_keepalive_fd >= 0) {
        ioctl(s_fb_keepalive_fd, FBIOBLANK, FB_BLANK_UNBLANK);
        close(s_fb_keepalive_fd);
        s_fb_keepalive_fd = -1;
    }
}

const char * platform_name(void)
{
    return "fbdev";
}
