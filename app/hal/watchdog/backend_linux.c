/**
 * @file backend_linux.c
 * 看门狗 Linux 后端：/dev/watchdog 真狗（超时自动复位设备）。
 *
 * Linux watchdog API 要点（换任何板子都适用）：
 *   open("/dev/watchdog")        —— 打开即开始计时，此后必须在超时内喂狗
 *   ioctl(WDIOC_SETTIMEOUT, &s)  —— 设定超时（**单位是秒**，驱动可能向上取整）
 *   ioctl(WDIOC_KEEPALIVE, 0)    —— 喂狗（也可 write 任意字节）
 *   write(fd, "V", 1) + close()  —— magic close：告诉驱动「我要正常停表」
 *                                   漏掉这一步，关闭 fd 后看门狗继续计时 → 设备被复位
 *
 * ★ 为什么 stop 必须写 'V'：应用正常退出 / 被 supervisor 重启时，若不停止计时，
 *   硬件会在超时后复位整块板子——表现为「重启应用却变成整机重启」。这是本模块最容易踩的坑。
 *
 * 设备路径可用环境变量 SAFE_WATCHDOG_DEV 覆盖（便于单测指向临时设备）。
 */
#include "watchdog_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/watchdog.h>

static int      s_fd = -1;
static uint32_t s_timeout_ms;
static uint32_t s_kicks;

static safe_err_t linux_init(uint32_t timeout_ms)
{
    const char * dev = getenv("SAFE_WATCHDOG_DEV");
    if(dev == NULL || *dev == '\0') dev = "/dev/watchdog";

    s_fd = open(dev, O_WRONLY);
    if(s_fd < 0) {
        printf("[WATCHDOG] 打开 %s 失败: %s（尝试软狗/占位后端）\n", dev, strerror(errno));
        return SAFE_ERR_UNSUP;
    }

    /* WDIOC_SETTIMEOUT 单位是秒：向上取整，最小 1 秒。 */
    int sec = (int)((timeout_ms + 999u) / 1000u);
    if(sec < 1) sec = 1;
    if(ioctl(s_fd, WDIOC_SETTIMEOUT, &sec) < 0) {
        printf("[WATCHDOG] 设定超时失败: %s（沿用驱动默认）\n", strerror(errno));
    }
    s_timeout_ms = timeout_ms;
    printf("[WATCHDOG] 设备=%s 超时=%ds\n", dev, sec);
    return SAFE_OK;
}

/* start 与 kick 行为一致（喂第一口狗即进入喂狗周期），分开只是为了语义清晰。 */
static safe_err_t do_kick(void)
{
    if(s_fd < 0) return SAFE_ERR_STATE;
    if(ioctl(s_fd, WDIOC_KEEPALIVE, 0) < 0) {
        /* 部分驱动只实现 write 语义，回退一次。 */
        if(write(s_fd, "\0", 1) != 1) return SAFE_ERR_FAIL;
    }
    s_kicks++;
    return SAFE_OK;
}

static safe_err_t linux_start(void)
{
    if(s_fd < 0) return SAFE_ERR_STATE;
    return do_kick();
}

static safe_err_t linux_stop(void)
{
    if(s_fd < 0) return SAFE_ERR_STATE;
    /* magic close：写 'V' 后驱动停止计时（不写则关闭 fd 后会复位设备）。 */
    if(write(s_fd, "V", 1) != 1) {
        printf("[WATCHDOG] magic close 失败: %s\n", strerror(errno));
        return SAFE_ERR_FAIL;
    }
    return SAFE_OK;
}

static void linux_deinit(void)
{
    if(s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

static uint32_t linux_kick_count(void)
{
    return s_kicks;
}

const hal_watchdog_backend_t hal_watchdog_backend_linux = {
    "linux",
    linux_init,
    linux_start,
    do_kick,
    linux_stop,
    linux_deinit,
    linux_kick_count,
};
