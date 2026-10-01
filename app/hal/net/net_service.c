/**
 * @file net_service.c
 * WLAN 服务门面：后端选择与能力缓存。
 *
 * 为什么不把实现直接写在这里：与 face / camera 两个 HAL 保持同一种形状
 * （抽象层 + 可替换后端），新增后端不用碰业务代码。
 *
 * 后端选择顺序：
 *   1) SAFE_NET_BACKEND 环境变量显式指定（wpa / none），CI 与单测用得上；
 *   2) 否则首选 wpa 真实后端；
 *   3) 首选后端不可用（探测失败）时**自动降级**到 none，并把降级结果记录在案，
 *      由 hal_net_backend_name() 暴露给开发者选项页——静默降级必须可观测，
 *      否则板上"连不上 WiFi"会变成查不出来的黑盒。
 *
 * available() 为什么要缓存：它要真的连一次 socket 才准，而 UI 每帧都可能问一次。
 * 缓存 --1 表示未探测，0/1 是结论。
 */
#include <stdlib.h>
#include <string.h>

#include "core/err.h"
#include "hal/hal_net.h"

extern const net_backend_t net_backend_wpa;
extern const net_backend_t net_backend_none;

static const net_backend_t * s_be = NULL;
static const net_backend_t * s_real_be = NULL;   /* 缓存"真实生效"的后端名用 */
static int s_available = -1;                     /* -1 未探测 */

void hal_net_set_backend(const net_backend_t * be)
{
    s_be = be;
    s_real_be = NULL;
    s_available = -1;
}

static const net_backend_t * current(void)
{
    if(s_be != NULL) return s_be;

    const char * env = getenv("SAFE_NET_BACKEND");
    if(env != NULL && *env != '\0') {
        if(strcmp(env, "none") == 0)       s_be = &net_backend_none;
        else if(strcmp(env, "wpa") == 0)   s_be = &net_backend_wpa;
        else                               s_be = &net_backend_wpa;   /* 未知值按 wpa */
    }
    else {
        s_be = &net_backend_wpa;
    }
    return s_be;
}

/* 返回真正提供能力的后端：首选不可用则退到 none。 */
static const net_backend_t * effective(void)
{
    current();
    if(s_real_be != NULL) return s_real_be;

    if(s_be->available != NULL && s_be->available()) {
        s_real_be = s_be;
    }
    else {
        s_real_be = &net_backend_none;   /* 降级 */
    }
    return s_real_be;
}

int hal_net_available(void)
{
    if(s_available >= 0) return s_available;

    const net_backend_t * be = effective();
    s_available = (be->available != NULL && be->available()) ? 1 : 0;
    return s_available;
}

const char * hal_net_backend_name(void)
{
    return effective()->name;
}

int hal_net_scan(wpa_ap_t * out, size_t cap)
{
    const net_backend_t * be = effective();
    if(be->scan == NULL) return SAFE_ERR_UNSUP;
    return be->scan(out, cap);
}

int hal_net_status(net_status_t * st)
{
    const net_backend_t * be = effective();
    if(st == NULL) return SAFE_ERR_PARAM;
    if(be->status == NULL) return SAFE_ERR_UNSUP;
    return be->status(st);
}

int hal_net_saved(net_saved_t * out, size_t cap)
{
    const net_backend_t * be = effective();
    if(out == NULL && cap > 0) return SAFE_ERR_PARAM;
    if(cap == 0) return 0;
    if(be->saved == NULL) return SAFE_ERR_UNSUP;
    return be->saved(out, cap);
}

int hal_net_connect(const char * ssid, const char * psk)
{
    const net_backend_t * be = effective();
    if(be->connect == NULL) return SAFE_ERR_UNSUP;
    return be->connect(ssid, psk);
}

int hal_net_disconnect(void)
{
    const net_backend_t * be = effective();
    if(be->disconnect == NULL) return SAFE_ERR_UNSUP;
    return be->disconnect();
}

