/**
 * @file hal_net.h
 * 无线网卡的操作：扫热点、连热点、查状态。
 *
 * ★ 这些函数都是**阻塞**的，扫描要等 0.5~5 秒。
 *   所以必须在后台 worker 线程调用，**不能在界面线程直接调**，否则画面会卡住。
 *   网络页已经按这个约定写了（worker_post 提交作业）。
 *
 * 这一层只管连 Wi-Fi（认证和关联），不管分配 IP ——
 * IP 是系统里的 DHCP 客户端拿的，这里只是把结果读出来显示。
 */
#pragma once

#include <stddef.h>

#include "core/net/net_scan_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STATE_UNKNOWN    = 0,   /* 查不到（后端不可用 / 读取失败） */
    NET_STATE_DOWN,             /* 没连任何热点 */
    NET_STATE_SCANNING,         /* 正在扫描 */
    NET_STATE_CONNECTING,       /* 配置已下发，正在握手 */
    NET_STATE_CONNECTED,        /* 已连上（不一定已拿到 IP） */
} net_state_t;

typedef struct {
    net_state_t state;
    char        ssid[WPA_SSID_MAX + 1];   /* 当前连着的热点名，不知道就空串 */
    char        ip[46];                   /* IPv4 / IPv6 地址，没拿到就空串 */
    int         signal_dbm;               /* 信号强度，负值；未知为 0 */
} net_status_t;

/* 当前有没有可用的无线网卡（第一次调用会真去探测，之后用缓存）。 */
int  hal_net_available(void);

/* 后端名字（"wpa" / "none"），开发者选项页显示用。 */
const char * hal_net_backend_name(void);

/* ---------------- 下面这些都要在后台线程调 ---------------- */

/* 扫描并等结果。返回扫到几个热点，失败返回负错误码。 */
int  hal_net_scan(wpa_ap_t * out, size_t cap);

/* 查当前连接状态。成功返回 SAFE_OK 并填好 st。 */
int  hal_net_status(net_status_t * st);

/* 读 wpa_supplicant 里已保存的网络清单。返回条数，失败返回负错误码。
 * 这是「真正会拿去连的配置」，和 store 里加密备份的密码是两回事，以前者为准。 */
int  hal_net_saved(net_saved_t * out, size_t cap);

/* 连接热点。psk 传 NULL 或空串表示开放网络。
 * ★ 返回成功只代表「配置交给 wpa_supplicant 了」，这时多半还没连上，
 *   要由调用方轮询 hal_net_status() 确认——无线连接天生是异步的。 */
int  hal_net_connect(const char * ssid, const char * psk);

/* 断开并删掉当前热点的配置。后端不支持返回 SAFE_ERR_UNSUP。 */
int  hal_net_disconnect(void);

/* 后端函数表（只有 net_service 用，业务层不用管）。 */
typedef struct {
    const char * name;
    int (*available)(void);
    int (*scan)(wpa_ap_t * out, size_t cap);
    int (*status)(net_status_t * st);
    int (*saved)(net_saved_t * out, size_t cap);
    int (*connect)(const char * ssid, const char * psk);
    int (*disconnect)(void);
} net_backend_t;

void hal_net_set_backend(const net_backend_t * be);

#ifdef __cplusplus
}
#endif
