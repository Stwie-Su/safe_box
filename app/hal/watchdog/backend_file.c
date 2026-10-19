/**
 * @file backend_file.c
 * 软件看门狗后端：把心跳写到文件，由 supervisor 检查文件时间戳。
 *
 * 【为什么需要这个后端】
 *   板子没有 /dev/watchdog（没有外置看门狗芯片，或内核没开 SoC 内置 WDOG）时，
 *   硬件看门狗这一层完全消失：supervisor 只能管「进程死了」，
 *   管不了「进程还活着但主循环卡死」。软狗用
 *   「应用周期写心跳 + 守护进程检查心跳是否过期」补上这一段。
 *
 * 【协议】
 *   应用：本后端每次 kick 往文件写一行**单调时钟**毫秒值（CLOCK_MONOTONIC）；
 *   守护：supervisor --heartbeat 指向同一路径，读该数值并与自己的单调时钟比较。
 *
 * ★ 为什么写单调时钟而不是墙上时间 / 用文件 mtime 判：
 *     这台设备有 DS3231 RTC 校时，还有一条远程 sync_time 指令能把系统时钟拨到任意点。
 *     用墙上时钟做超时判定的话，把时钟往未来一拨，软狗就永远判不出卡死
 *     （心跳时间比"现在"还新），等于被一条远程指令绕过。
 *     单调时钟不受校时影响，两边是同一内核的同一基准，比较才有意义。
 *
 * 【能力边界（必须写清楚，别把它当硬件狗）】
 *   覆盖：主循环卡死、死锁、线程饿死、业务代码死循环 —— 心跳停 → supervisor 重启。
 *   不覆盖：内核 panic / 整机死机 / supervisor 自身死亡 / 断电。
 *           这些只有硬件看门狗（SoC 内置 WDOG 或外置芯片）能复位整机。
 *   因此：有真狗时优先用 linux 后端；软狗是「没有真狗时的降级方案」，不是等价替代。
 *
 * 【为什么不用 fsync】
 *   supervisor 与本机同一文件系统，pwrite 返回后读侧已经能看到新内容；
 *   每 1s fsync 一次会白白磨损 flash，且拖慢主循环。
 *   （心跳这种"丢了也就晚一拍"的数据，不值得为它付出 fsync 的代价。）
 */
#include "watchdog_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HEARTBEAT_FILE "/var/run/safe_heartbeat"

static int      s_fd = -1;
static char     s_path[256];
static uint32_t s_kicks;

static safe_err_t file_init(uint32_t timeout_ms)
{
    const char * p = getenv("SAFE_WATCHDOG_FILE");
    if(p == NULL || *p == '\0') p = DEFAULT_HEARTBEAT_FILE;

    snprintf(s_path, sizeof(s_path), "%s", p);
    s_fd = open(s_path, O_WRONLY | O_CREAT, 0644);
    if(s_fd < 0) {
        /* 路径不可写（PC 普通用户 /var/run、只读根文件系统）→ 交给分发层降级 null。
         * 这里必须报错而不是静默成功，否则「以为有软狗、其实没写」最难查。 */
        printf("[WATCHDOG] 软狗心跳文件打不开 %s: %s\n", s_path, strerror(errno));
        return SAFE_ERR_IO;
    }
    printf("[WATCHDOG] 软狗后端：心跳文件=%s timeout=%ums（由 supervisor 检查）\n",
           s_path, timeout_ms);
    return SAFE_OK;
}

/* 写毫秒时间戳；文件偏移归零，保证文件不会随运行时间无限增长。 */
static safe_err_t file_kick(void)
{
    if(s_fd < 0) return SAFE_ERR_STATE;

    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return SAFE_ERR_FAIL;

    long long ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld\n", ms);

    if(pwrite(s_fd, buf, (size_t)n, 0) != n) return SAFE_ERR_IO;

    s_kicks++;
    return SAFE_OK;
}

static safe_err_t file_start(void)
{
    return file_kick();
}

static safe_err_t file_stop(void)
{
    if(s_fd < 0) return SAFE_ERR_STATE;

    /* 留下 stopped 标记：正常退出时 supervisor 看到它就不再按「心跳过期」处理，
     * 避免「关应用被当成卡死、又拉起来」的误判。
     * 先截断再写：文件里原有时间戳比 "stopped\n" 长，不截断会留下尾巴。 */
    if(ftruncate(s_fd, 0) != 0) { /* 截断失败仍继续写，最多留脏尾巴，不影响判定 */ }
    if(pwrite(s_fd, "stopped\n", 8, 0) != 8) { /* 写失败不阻断关闭流程 */ }
    return SAFE_OK;
}

static void file_deinit(void)
{
    if(s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

static uint32_t file_kick_count(void)
{
    return s_kicks;
}

const hal_watchdog_backend_t hal_watchdog_backend_file = {
    "file",
    file_init,
    file_start,
    file_kick,
    file_stop,
    file_deinit,
    file_kick_count,
};
