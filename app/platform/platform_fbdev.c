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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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

/* ------------------------------------------------------------------
 * 函数：fb_warn_other_writers（文件内私有）
 * 功能：扫描 /proc，报告除本进程外还有谁打开着同一个 fb 设备。
 * 调用者：platform_init_io()，每次启动扫一次。
 *
 * ★ 为什么需要这道自检
 *   fbdev 没有独占语义：第二个进程照样能 open 并绘制，两个写者各画各的，
 *   画面互相覆盖且**没有任何报错**。上层看到的只是「UI 花了 / 少了一整块」，
 *   很像渲染 bug，实际是另一个程序在抢屏（本板出厂 demo mxapp2 就是这样：
 *   它由 /etc/init.d/S99myirhmi2 拉起，一直占着 fb0 与触摸 event1）。
 *   只提示不拦截：生产环境不该因为一行日志把应用打挂，但必须留下判据。
 * ------------------------------------------------------------------ */
static void fb_warn_other_writers(const char * fbdev)
{
    /* 只比文件名：readlink 给出的是 /dev/fb0，与配置里的写法可能有前缀差异 */
    const char * base = strrchr(fbdev, '/');
    base = (base != NULL) ? base + 1 : fbdev;

    DIR * proc = opendir("/proc");
    if(proc == NULL) return;

    const pid_t self = getpid();
    int hits = 0;
    struct dirent * de;

    while((de = readdir(proc)) != NULL) {
        char fd_dir[64];
        DIR * fds = NULL;
        struct dirent * fe;
        int  found = 0;
        long pid;

        if(de->d_name[0] < '0' || de->d_name[0] > '9') continue;    /* 只要 pid 目录 */

        snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", de->d_name);
        fds = opendir(fd_dir);
        if(fds == NULL) continue;      /* 进程刚退出 / 无权限：跳过，不是错误 */

        while((fe = readdir(fds)) != NULL) {
            char path[160];
            char link[128];
            const char * lb;
            ssize_t n;

            if(fe->d_name[0] == '.') continue;

            snprintf(path, sizeof(path), "%s/%s", fd_dir, fe->d_name);
            n = readlink(path, link, sizeof(link) - 1);
            if(n <= 0) continue;
            link[n] = '\0';

            lb = strrchr(link, '/');
            lb = (lb != NULL) ? lb + 1 : link;
            if(strcmp(lb, base) == 0) { found = 1; break; }
        }
        closedir(fds);
        if(!found) continue;

        pid = strtol(de->d_name, NULL, 10);
        if((pid_t)pid == self) continue;   /* 自己（含保活句柄）不算 */

        {
            char exe_link[64];
            char exe[128];
            ssize_t m;

            snprintf(exe_link, sizeof(exe_link), "/proc/%s/exe", de->d_name);
            m = readlink(exe_link, exe, sizeof(exe) - 1);
            if(m > 0) {
                exe[m] = '\0';
            }
            else {
                snprintf(exe, sizeof(exe), "?");
            }
            LV_LOG_USER("fbdev: 警告：另一个进程也在写 %s —— pid=%ld (%s)，画面会互相覆盖",
                        fbdev, pid, exe);
        }
        hits++;
    }
    closedir(proc);

    if(hits == 0) {
        LV_LOG_USER("fbdev: %s 只有本进程在写", fbdev);
    }
    else {
        LV_LOG_USER("fbdev: 共 %d 个抢屏进程，查开机脚本是否还启动了别的 UI", hits);
    }
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

    /* 抢屏自检：fbdev 允许多个写者，画面互相覆盖却不报错，必须显式查一次 */
    {
        const char * fbdev = getenv("LV_LINUX_FBDEV_DEVICE");
        fb_warn_other_writers((fbdev != NULL && *fbdev != '\0') ? fbdev : "/dev/fb0");
    }
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
