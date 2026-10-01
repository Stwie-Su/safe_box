/**
 * @file backend_none.c
 * WLAN「无后端」实现：所有操作返回 UNSUP，available() 返回 0。
 *
 * 存在的意义（不是占位）：让「没有无线网卡」成为一条**被支持的正常状态**，
 * 而不是运行时错误。PC 开发机上通常没有可管的无线网卡，板上也可能需要跑
 * 纯有线（以太网）形态。UI 拿到 available()==0 就显示「无无线网卡」，
 * 而不是靠 try/catch 式地反复失败。
 */
#include "core/err.h"
#include "hal/hal_net.h"

static int none_available(void)
{
    return 0;
}

static int none_scan(wpa_ap_t * out, size_t cap)
{
    (void)out;
    (void)cap;
    return SAFE_ERR_UNSUP;
}

static int none_status(net_status_t * st)
{
    if(st == NULL) return SAFE_ERR_PARAM;
    st->state      = NET_STATE_UNKNOWN;
    st->ssid[0]    = '\0';
    st->ip[0]      = '\0';
    st->signal_dbm = 0;
    return SAFE_ERR_UNSUP;
}

static int none_saved(net_saved_t * out, size_t cap)
{
    (void)out;
    (void)cap;
    return SAFE_ERR_UNSUP;
}

static int none_connect(const char * ssid, const char * psk)
{
    (void)ssid;
    (void)psk;
    return SAFE_ERR_UNSUP;
}

static int none_disconnect(void)
{
    return SAFE_ERR_UNSUP;
}

const net_backend_t net_backend_none = {
    .name       = "none",
    .available  = none_available,
    .scan       = none_scan,
    .status     = none_status,
    .saved      = none_saved,
    .connect    = none_connect,
    .disconnect = none_disconnect,
};

