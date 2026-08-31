/**
 * @file time_sys.c
 * 系统时间后端：直接读 C 库时间。
 *
 * 无 RTC 时的默认实现。校时走 settimeofday，进程需要有相应权限。
 */

#include "time_backend.h"

#include <sys/time.h>
#include <time.h>

static uint32_t sys_now(void)
{
    return (uint32_t)time(NULL);
}

static safe_err_t sys_set(uint32_t unix_seconds)
{
    struct timeval tv;
    tv.tv_sec  = (time_t)unix_seconds;
    tv.tv_usec = 0;
    if(settimeofday(&tv, NULL) != 0) return SAFE_ERR_PERM;
    return SAFE_OK;
}

const hal_time_backend_t hal_time_backend_sys = {
    "sys",
    TIME_SRC_SYS,
    sys_now,
    sys_set,
};
