/**
 * @file test_watchdog_file.c
 * 软狗后端单测：心跳文件创建 / kick 更新 mtime / stop 留标记 / 失败降级。
 *
 * 为什么单独测这一层：板上没有 /dev/watchdog，硬狗那套 PC 上验不了；
 * 软狗是「无真狗时唯一能覆盖主循环卡死」的机制，必须能确定性验证。
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hal/hal_watchdog.h"

static int fails = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while(0)

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 读心跳文件里的数值（单调时钟毫秒）；读不到返回 -1。 */
static long long read_beat(const char * path)
{
    FILE * f = fopen(path, "r");
    if(f == NULL) return -1;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if(n == 0) return -1;
    if(strncmp(buf, "stopped", 7) == 0) return -2;   /* 正常退出标记 */
    return strtoll(buf, NULL, 10);
}

int main(void)
{
    char path[] = "/tmp/safe_hb_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if(fd >= 0) close(fd);

    /* ---- 1. 正常路径：init / start 后文件存在且有内容 ---- */
    setenv("SAFE_WATCHDOG_BACKEND", "file", 1);
    setenv("SAFE_WATCHDOG_FILE", path, 1);
    setenv("SAFE_WATCHDOG_KICK_MS", "1", 1);   /* 关掉 1s 节流，便于验证每次 kick 都落盘 */

    CHECK(hal_watchdog_init(5000) == SAFE_OK);
    CHECK(hal_watchdog_start() == SAFE_OK);

    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(hal_watchdog_kick_count() >= 1);

    /* ---- 2. kick 推进心跳数值（软狗判定就靠它）----
     * 判据必须是「内容里的单调时钟值」，不是文件 mtime：
     * 这台设备有 RTC 校时和远程 sync_time，墙上时钟会被拨动。 */
    long long before = read_beat(path);
    CHECK(before > 0);

    sleep_ms(50);
    CHECK(hal_watchdog_kick() == SAFE_OK);
    long long after = read_beat(path);
    CHECK(after > before);                    /* 心跳值向前推进 */

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    long long now = (long long)ts_now.tv_sec * 1000LL + ts_now.tv_nsec / 1000000L;
    CHECK(after <= now);                      /* 不能写未来时间 */
    CHECK(now - after < 1000);                /* 且与本机单调时钟同源 */

    /* ---- 3. stop 留 stopped 标记：正常退出不能被当成卡死 ---- */
    CHECK(hal_watchdog_stop() == SAFE_OK);
    char buf[16] = {0};
    FILE * f = fopen(path, "r");
    CHECK(f != NULL);
    if(f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n >= 7);
        CHECK(strncmp(buf, "stopped", 7) == 0);
    }
    hal_watchdog_deinit();

    /* ---- 4. 心跳文件不可写 → init 降级 null，但不返回错误（系统仍能起来）---- */
    setenv("SAFE_WATCHDOG_FILE", "/no/such/dir/safe_heartbeat", 1);
    CHECK(hal_watchdog_init(5000) == SAFE_OK);   /* 指定后端失败 → 降级 null */
    CHECK(hal_watchdog_start() == SAFE_OK);
    CHECK(hal_watchdog_kick() == SAFE_OK);       /* null 后端照常计数，不产生副作用 */
    hal_watchdog_stop();
    hal_watchdog_deinit();

    /* ---- 5. 未知后端名 → 降级 null，不崩 ---- */
    setenv("SAFE_WATCHDOG_BACKEND", "no_such_backend", 1);
    CHECK(hal_watchdog_init(1000) == SAFE_OK);
    hal_watchdog_deinit();

    unlink(path);
    unsetenv("SAFE_WATCHDOG_BACKEND");
    unsetenv("SAFE_WATCHDOG_FILE");
    unsetenv("SAFE_WATCHDOG_KICK_MS");

    if(fails) { printf("[FAIL] %s（%d 项未过）\n", __FILE__, fails); return 1; }
    printf("[PASS] %s\n", __FILE__);
    return 0;
}
