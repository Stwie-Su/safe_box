/**
 * @file backend_gpio.c
 * 执行器 GPIO 后端（sysfs）：继电器高脉冲开锁，到点自动拉低。
 *
 * 【为什么要这个后端】
 *   backend_mock 只打印日志，板上接线之后锁不会动。本后端把
 *   「拉高 N 毫秒再拉低」落到 /sys/class/gpio 上，其余接口与 mock 完全一致，
 *   所以 auth_fsm / UI 一行都不用改。
 *
 * 【安全方向】
 *   一切失败都往「低电平（保持锁闭）」收：
 *     init   失败 → 返回错误，由分发层回退 mock 并打日志，绝不留下悬空的高电平；
 *     pulse  失败 → 立刻写 0 再返回错误；
 *     release   → 先让在途的关断线程失效（递增世代号），再写 0。
 *   脉冲上限 500ms 由分发层截断，本层不重复防御。
 *
 * 【可测试性】
 *   路径与引脚编号都可用环境变量覆盖，测试用临时目录造一棵假 sysfs 树即可：
 *     SAFE_GPIO_ROOT=/tmp/xxx        （默认 /sys/class/gpio）
 *     SAFE_ACTUATOR_GPIO=115         （默认 115：GPIO4_19 = (4-1)*32 + 19，
 *                                     以板上实际编号为准，换接线要改这里）
 */

#include "platform/thread_util.h"      /* ★ 必须第一个 include：它要在 <pthread.h> 前定义 _GNU_SOURCE */

#include "actuator_backend.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GPIO_ROOT_DEFAULT    "/sys/class/gpio"
#define GPIO_PIN_DEFAULT     "115"
#define GPIO_EXPORT_WAIT_MS  200

static char          s_root[128];
static unsigned      s_pin;
static char          s_value_path[192];
static volatile bool s_busy;
static volatile unsigned s_gen;          /* 世代号：让 release 之后的旧关断线程失效 */

typedef struct {
    uint32_t ms;
    unsigned gen;
} off_job_t;

/* 写文件（sysfs 的 export/direction/value 都是 write-only 的属性文件） */
static int write_file(const char * path, const char * val)
{
    int fd = open(path, O_WRONLY);
    if(fd < 0) return -1;

    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

static void * off_thread_fn(void * arg)
{
    off_job_t * job = (off_job_t *)arg;

    safe_thread_setname("safe-act-off");
    usleep(job->ms * 1000u);

    /* 世代号变了说明期间发生过 release 或新的脉冲，这一拍不该再动手 */
    if(job->gen == s_gen) {
        (void)write_file(s_value_path, "0");
        printf("[ACTUATOR] gpio pulse end after %u ms -> LOW\n", (unsigned)job->ms);
        hal_actuator_notify_low();
        s_busy = false;
    }
    free(job);
    return NULL;
}

static safe_err_t gpio_init(void)
{
    const char * root = getenv("SAFE_GPIO_ROOT");
    const char * pin  = getenv("SAFE_ACTUATOR_GPIO");
    char         path[192];
    int          waited = 0;

    if(root == NULL || *root == '\0') root = GPIO_ROOT_DEFAULT;
    if(pin == NULL || *pin == '\0')   pin  = GPIO_PIN_DEFAULT;

    snprintf(s_root, sizeof(s_root), "%s", root);
    s_pin = (unsigned)strtoul(pin, NULL, 10);
    s_busy = false;
    s_gen++;

    /* export：内核在引脚已经导出时返回 EBUSY，写失败不算错，下面看目录在不在 */
    snprintf(path, sizeof(path), "%s/export", s_root);
    (void)write_file(path, pin);

    snprintf(path, sizeof(path), "%s/gpio%u", s_root, s_pin);
    while(access(path, F_OK) != 0 && waited < GPIO_EXPORT_WAIT_MS) {
        usleep(10 * 1000);
        waited += 10;
    }
    if(access(path, F_OK) != 0) {
        printf("[ACTUATOR] gpio 初始化失败：%s 不存在（引脚未导出，或被设备树里其它驱动占用）\n", path);
        return SAFE_ERR_IO;
    }

    snprintf(s_value_path, sizeof(s_value_path), "%s/gpio%u/value", s_root, s_pin);
    snprintf(path, sizeof(path), "%s/gpio%u/direction", s_root, s_pin);
    if(write_file(path, "out") != 0) {
        printf("[ACTUATOR] gpio 设置方向失败：%s\n", path);
        return SAFE_ERR_IO;
    }
    /* 初始必须是低电平：上电抖动或上次异常退出留下的高电平都不能带进业务 */
    if(write_file(s_value_path, "0") != 0) {
        printf("[ACTUATOR] gpio 初始拉低失败：%s\n", s_value_path);
        return SAFE_ERR_IO;
    }

    printf("[ACTUATOR] gpio backend ready pin=%u root=%s 初始=LOW\n", s_pin, s_root);
    return SAFE_OK;
}

static safe_err_t gpio_pulse(uint32_t ms)
{
    off_job_t * job;
    pthread_t   th;

    if(s_busy) return SAFE_ERR_BUSY;

    if(write_file(s_value_path, "1") != 0) {
        printf("[ACTUATOR] gpio 拉高失败：%s\n", s_value_path);
        return SAFE_ERR_IO;
    }

    job = (off_job_t *)malloc(sizeof(*job));
    if(job == NULL) {
        (void)write_file(s_value_path, "0");
        return SAFE_ERR_FAIL;
    }
    job->ms  = ms;
    job->gen = ++s_gen;

    s_busy = true;
    if(pthread_create(&th, NULL, off_thread_fn, job) != 0) {
        free(job);
        s_busy = false;
        (void)write_file(s_value_path, "0");
        return SAFE_ERR_FAIL;
    }
    pthread_detach(th);
    return SAFE_OK;
}

static safe_err_t gpio_release(void)
{
    s_gen++;              /* 让在途的关断线程失效，避免它稍后又写一次 */
    s_busy = false;
    if(write_file(s_value_path, "0") != 0) return SAFE_ERR_IO;
    hal_actuator_notify_low();
    return SAFE_OK;
}

const hal_actuator_backend_t hal_actuator_backend_gpio = {
    "gpio",
    gpio_init,
    gpio_pulse,
    gpio_release,
};
