/**
 * @file test_actuator_gpio.c
 * 执行器 GPIO 后端单测：方向/初始电平/脉冲时序/强制拉低/降级。
 *
 * 为什么用假 sysfs 树：板上的真锁还没接，真机验不了「拉高多久、有没有拉低」。
 * 把 SAFE_GPIO_ROOT 指到临时目录，造出 export / gpioN/direction / gpioN/value
 * 三个普通文件，就能把**时序与写入顺序**确定性验证掉 —— 这层最容易出的错
 * （忘记拉低、写得不是 "out"、失败后留高电平）都在这里拦住。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hal/hal_actuator.h"

static int fails = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while(0)

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 读一个 sysfs 属性文件的全文（后端写的是 "0"/"1"/"out"，不带换行） */
static void read_all(const char * path, char * out, size_t cap)
{
    FILE * f = fopen(path, "r");
    out[0] = '\0';
    if(f == NULL) return;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
}

static void touch(const char * path)
{
    FILE * f = fopen(path, "w");
    if(f != NULL) fclose(f);
}

int main(void)
{
    char root[] = "/tmp/safe_gpio_test_XXXXXX";
    char dir[192], path[224];
    char buf[32];

    if(mkdtemp(root) == NULL) { printf("FAIL: mkdtemp\n"); return 1; }

    snprintf(path, sizeof(path), "%s/export", root);
    touch(path);
    snprintf(dir, sizeof(dir), "%s/gpio115", root);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/direction", dir);
    touch(path);
    snprintf(path, sizeof(path), "%s/value", dir);
    touch(path);

    setenv("SAFE_GPIO_ROOT", root, 1);
    setenv("SAFE_ACTUATOR_GPIO", "115", 1);
    setenv("SAFE_ACTUATOR_BACKEND", "gpio", 1);

    printf("== 1. 初始化：导出、方向 out、初始 LOW ==\n");
    CHECK(hal_actuator_init() == SAFE_OK);
    snprintf(path, sizeof(path), "%s/export", root);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "115") == 0);
    snprintf(path, sizeof(path), "%s/direction", dir);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "out") == 0);
    snprintf(path, sizeof(path), "%s/value", dir);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0);
    CHECK(hal_actuator_state() == false);

    printf("== 2. 脉冲：立即 HIGH，到点自动 LOW ==\n");
    CHECK(hal_actuator_pulse(60) == SAFE_OK);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0);
    CHECK(hal_actuator_state() == true);
    sleep_ms(160);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0);
    CHECK(hal_actuator_state() == false);

    printf("== 3. 超过 500ms 的请求被分发层截断 ==\n");
    CHECK(hal_actuator_pulse(5000) == SAFE_OK);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0);
    sleep_ms(700);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0);      /* 500ms 就该落下来，没等到 5s */

    printf("== 4. 强制拉低：在途脉冲被打断，且不会被旧的关断线程覆盖 ==\n");
    CHECK(hal_actuator_pulse(300) == SAFE_OK);
    sleep_ms(50);
    CHECK(hal_actuator_release() == SAFE_OK);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0);
    CHECK(hal_actuator_state() == false);
    /* 立刻发第二个脉冲：旧的关断线程到点后不应该把它拉低 */
    CHECK(hal_actuator_pulse(400) == SAFE_OK);
    sleep_ms(320);                     /* 跨过第一个脉冲的 300ms 截止点 */
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0);
    sleep_ms(200);
    read_all(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0);

    printf("== 5. 显式 gpio 但路径不可用：init 必须报错（不能假装成功）==\n");
    setenv("SAFE_GPIO_ROOT", "/nonexistent_gpio_root", 1);
    CHECK(hal_actuator_init() != SAFE_OK);

    printf("== 6. 未指定后端且 GPIO 不可用：自动回退 mock，仍可用 ==\n");
    unsetenv("SAFE_ACTUATOR_BACKEND");
    CHECK(hal_actuator_init() == SAFE_OK);
    CHECK(hal_actuator_pulse(30) == SAFE_OK);
    CHECK(hal_actuator_state() == true);
    sleep_ms(120);
    CHECK(hal_actuator_state() == false);

    /* 清理 */
    snprintf(path, sizeof(path), "%s/direction", dir); unlink(path);
    snprintf(path, sizeof(path), "%s/value", dir);     unlink(path);
    rmdir(dir);
    snprintf(path, sizeof(path), "%s/export", root);   unlink(path);
    rmdir(root);

    if(fails != 0) { printf("[FAIL] test_actuator_gpio（%d 项）\n", fails); return 1; }
    printf("[PASS] test_actuator_gpio\n");
    return 0;
}
