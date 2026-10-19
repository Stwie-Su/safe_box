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

    /* ---- 2. kick 真的更新了 mtime（软狗判定就靠它）---- */
    struct timespec old[2] = { { 1000, 0 }, { 1000, 0 } };
    CHECK(utimensat(AT_FDCWD, path, old, 0) == 0);
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_mtime == 1000);               /* 先确认已改到过去 */

    sleep_ms(5);
    CHECK(hal_watchdog_kick() == SAFE_OK);
    CHECK(stat(path, &st) == 0);
    CHECK(st.st_mtime > 1000);                /* 心跳把时间拉回现在 */

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
