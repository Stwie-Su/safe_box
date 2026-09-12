/**
 * @file time_rtc.c
 * 外部 RTC 时间后端（DS3231 到货后启用，规约 §5.14）。
 *
 * 走 Linux RTC 子系统的字符设备 /dev/rtc<N>（DS3231 挂 I2C 后由内核 ds1307
 * 驱动接管并导出 /dev/rtc1；/dev/rtc0 是片上 SNVS，无备份电池、上电归零，
 * 不可用作持久化时间源）。全部读写只用到两个 ioctl：
 *   - RTC_RD_TIME ：读 struct rtc_time（BCD 解码由内核驱动完成）；
 *   - RTC_SET_TIME：写 struct rtc_time（tm_year 自 1900 起，tm_mon 0-11）。
 *
 * 与手写 I2C 的区别：不做寄存器寻址、不做 BCD 编解码、不与 hwclock 抢设备，
 * 复用内核驱动的电源域/寄存器语义，出错路径由内核统一兜住。
 *
 * 时间基准一律 UTC：RTC 寄存器内容按 UTC 解释，转 Unix 秒用自算的 civil-days
 * 公式（不用 timegm —— 非标准 C，且结果会受 TZ 影响），与 settimeofday 口径
 * 一致，TOTP 才对得上。
 *
 * 降级约定（§5.14 硬要求）：节点不存在 / 打不开 / ioctl 失败时**不崩不卡**——
 * 返回系统时间并打印一条限频（只打一次）的 [time] 告警。
 *
 * fd 策略：/dev/rtcN 是**独占**设备（rtc-dev 的 RTC_DEV_BUSY 位，板上实测），
 * 故首次成功打开后常驻复用，不做「每次读都开关」。详见 §5.14。
 */

#include "time_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* 默认节点：DS3231（ds1307 驱动）。rtc0 = SNVS，无电池，不用。 */
#define RTC_DEV_DEFAULT "/dev/rtc1"

const char * time_rtc_dev(void)
{
    const char * dev = getenv("SAFE_RTC_DEV");
    if(dev == NULL || *dev == '\0') dev = RTC_DEV_DEFAULT;
    return dev;
}

/* ---------------- 历法换算：UTC <-> Unix 秒 ---------------- */

/* 公历日期 -> 1970-01-01 起的天数（days_from_civil，纯整数，不依赖时区）。 */
static long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);                              /* 3 月为岁首 */
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;              /* [0, 399] */
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0, 365] */
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0, 146096] */
    return (long)era * 146097L + (long)doe - 719468L;
}

static uint32_t rtc_tm_to_unix(const struct rtc_time * t)
{
    const long days = days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    const long sec  = (long)t->tm_hour * 3600L + (long)t->tm_min * 60L + (long)t->tm_sec;
    const long v    = days * 86400L + sec;
    return (v < 0L) ? 0u : (uint32_t)v;   /* 1970 年前的荒唐值夹到 0，不外溢 */
}

/* Unix 秒 -> struct rtc_time（civil_from_days 的逆运算）。 */
static void unix_to_rtc_tm(uint32_t unix_seconds, struct rtc_time * t)
{
    const uint32_t days = unix_seconds / 86400u;
    const uint32_t rem  = unix_seconds % 86400u;

    t->tm_hour = (int)(rem / 3600u);
    t->tm_min  = (int)((rem % 3600u) / 60u);
    t->tm_sec  = (int)(rem % 60u);

    const uint32_t z   = days + 719468u;
    const uint32_t era = z / 146097u;
    const uint32_t doe = z - era * 146097u;                                  /* [0, 146096] */
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t y   = yoe + era * 400u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);         /* [0, 365] */
    const uint32_t mp  = (5u * doy + 2u) / 153u;                             /* [0, 11] */
    const uint32_t d   = doy - (153u * mp + 2u) / 5u + 1u;                   /* [1, 31] */
    const int      m   = (int)mp + (mp < 10u ? 3 : -9);                      /* [1, 12] */

    t->tm_mon   = m - 1;                                    /* 0-11 */
    t->tm_mday  = (int)d;
    t->tm_year  = (int)(y + (m <= 2 ? 1u : 0u)) - 1900;     /* 自 1900 起 */
    t->tm_wday  = 0;    /* 内核不校验，驱动自行重算 */
    t->tm_yday  = 0;
    t->tm_isdst = 0;
}

/* ---------------- 设备读写 ---------------- */

static safe_err_t rtc_read_fd(int fd, uint32_t * out_unix)
{
    struct rtc_time t;
    memset(&t, 0, sizeof(t));
    if(ioctl(fd, RTC_RD_TIME, &t) != 0) return SAFE_ERR_IO;
    *out_unix = rtc_tm_to_unix(&t);
    return SAFE_OK;
}

/* 常驻 fd：/dev/rtcN 是独占设备（rtc-dev 的 RTC_DEV_BUSY，见 §5.14），
 * 首次成功打开后一直持有并复用——若改成每次读都开关，节点一旦被别的进程
 * 占用（或独占位没能复原），后续每次 hal_time() 都会 EBUSY 而静默退化成
 * 系统时间。打开失败不缓存「失败」：下次调用仍会重试（驱动后挂也能自愈）。 */
static int s_fd = -1;

static int rtc_open_fd(void)
{
    if(s_fd >= 0) return s_fd;

    const char * dev = time_rtc_dev();
    int fd = open(dev, O_RDWR);
    if(fd < 0) return -1;
    s_fd = fd;
    return s_fd;
}

static void rtc_drop_fd(void)
{
    if(s_fd < 0) return;
    close(s_fd);
    s_fd = -1;
}

bool time_rtc_available(void)
{
    const int fd = rtc_open_fd();      /* 探测成功即保留，不再关闭 */
    if(fd < 0) return false;

    uint32_t v = 0;
    safe_err_t e = rtc_read_fd(fd, &v);
    if(e != SAFE_OK) {
        rtc_drop_fd();                 /* 能开不能读：当作不可用 */
        return false;
    }
    return true;
}

/* 运行期读失败限频：RTC 掉了（掉电 / 驱动卸载）时不能每次 hal_time() 都刷一行。 */
static bool s_read_warned;

static uint32_t rtc_now(void)
{
    const char * dev = time_rtc_dev();

    const int fd = rtc_open_fd();
    if(fd < 0) {
        if(!s_read_warned) {
            s_read_warned = true;
            printf("[time] RTC 读失败：open %s（%s），本次返回系统时间\n", dev, strerror(errno));
        }
        return (uint32_t)time(NULL);
    }

    uint32_t v = 0;
    safe_err_t e = rtc_read_fd(fd, &v);

    if(e != SAFE_OK) {
        rtc_drop_fd();                 /* 丢弃坏 fd，下次调用会重开（驱动重载可自愈） */
        if(!s_read_warned) {
            s_read_warned = true;
            printf("[time] RTC 读失败：ioctl RTC_RD_TIME on %s（%s），本次返回系统时间\n",
                   dev, strerror(errno));
        }
        return (uint32_t)time(NULL);
    }
    return v;
}

static safe_err_t rtc_set(uint32_t unix_seconds)
{
    const char * dev = time_rtc_dev();

    const int fd = rtc_open_fd();
    if(fd < 0) {
        printf("[time] RTC 校时失败：open %s（%s）\n", dev, strerror(errno));
        return SAFE_ERR_IO;
    }

    struct rtc_time t;
    memset(&t, 0, sizeof(t));
    unix_to_rtc_tm(unix_seconds, &t);

    if(ioctl(fd, RTC_SET_TIME, &t) != 0) {
        printf("[time] RTC 校时失败：ioctl RTC_SET_TIME on %s（%s）\n", dev, strerror(errno));
        rtc_drop_fd();
        return SAFE_ERR_PERM;
    }

    printf("[time] RTC 校时成功：unix=%u 写入 %s\n", unix_seconds, dev);
    return SAFE_OK;
}

const hal_time_backend_t hal_time_backend_rtc = {
    "rtc",
    TIME_SRC_RTC,
    rtc_now,
    rtc_set,
};
