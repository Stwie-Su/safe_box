/**
 * @file hal_net.h
 * 无线局域网（WLAN）硬件抽象层：扫描 / 连接 / 状态查询。
 *
 * 分层：本文件是 hal 的公共头文件，按 tools/check_layers.sh 第 4 条的纪律，
 * **不得出现平台实现头**（sys/ unistd linux/ ...），包含它的一方不会把平台
 * 依赖传播出去。实现分散在 net/backend_*.c，由 net_service.c 运行期选择。
 *
 * ★ 时序约定（重要）：本层所有接口都是**阻塞**的。
 *   - hal_net_scan() 会触发扫描并等待结果，实测要 0.5~5 秒；
 *   - hal_net_status()/connect() 走 ctrl_iface 往返，几十毫秒级。
 *   因此调用方必须把它们放进 core/support/worker.h 的后台作业里，
 *   **绝不能在 LVGL 主线程直接调用**——否则整帧刷新被卡住，UI 假死。
 *   page_network.c 已按此接线：worker_post(fn, arg, done)。
 *
 * ★ 边界（明示没做什么，避免误判能力）：
 *   - 本层**不负责 DHCP**。wpa_supplicant 只管认证与关联（WPA 握手），
 *     IP 地址由系统里的 DHCP 客户端（板子上通常是 udhcpc）获取；
 *     hal_net_status() 的 ip 字段是从 wpa_supplicant STATUS 里**读出来的结果**，
 *     不是本层申请的。
 *   - 本层不管理网卡的 up/down 与 rfkill，那属于系统初始化脚本的职责。
 */
#pragma once

#include <stddef.h>

#include "core/net/net_scan_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STATE_UNKNOWN    = 0,   /* 查询不到（后端不可用 / 读取失败） */
    NET_STATE_DOWN,             /* 未连接：接口在，但没有关联到任何 AP */
    NET_STATE_SCANNING,         /* 扫描进行中 */
    NET_STATE_CONNECTING,       /* 已下发配置，握手进行中 */
    NET_STATE_CONNECTED,        /* 已完成关联（未必已拿到 IP） */
} net_state_t;

typedef struct {
    net_state_t state;
    char        ssid[WPA_SSID_MAX + 1];   /* 当前关联的 SSID，未知则空串 */
    char        ip[46];                   /* IPv4/IPv6 字符串，未取到则空串 */
    int         signal_dbm;               /* 当前 AP 信号强度，未知为 0 */
} net_status_t;

/* ---------------- 能力询问 ---------------- */

/* 当前后端是否真的可用（有无线网卡且 ctrl_iface 能连上）。
 * 返回 1 可用 / 0 不可用。UI 据此决定显示真实结果还是「无无线网卡」。
 * 首次调用会做一次真实探测，之后缓存结果——探测有 IO，不该每帧都做。 */
int  hal_net_available(void);

/* 当前后端名（"wpa" / "none"），用于开发者选项页展示。永不为 NULL。 */
const char * hal_net_backend_name(void);

/* ---------------- 操作（均需在 worker 线程调用） ---------------- */

/* 触发扫描并等待结果。返回写入 out 的热点个数（0 = 一个都没扫到），
 * 失败返回负的 safe_err_t。cap 为 0 或 out 为 NULL 时返回 SAFE_ERR_PARAM。 */
int  hal_net_scan(wpa_ap_t * out, size_t cap);

/* 查询当前链路状态。成功返回 SAFE_OK 并填充 st；失败返回负错误码。 */
int  hal_net_status(net_status_t * st);

/* 读取 wpa_supplicant 里**已保存**的网络配置清单（LIST_NETWORKS）。
 * 返回写入 out 的条数（0 = 没有保存过任何网络），失败返回负错误码。
 * ★ 与 store 里加密存放的 PSK 库是两个不同的东西：这里是「真正会被用来连接」
 *   的配置，那里是「凭据备份」。二者应当同步，但不互为依据——以这里的为准，
 *   因为删了配置却留着凭据没有任何害处，反过来则会导致连不上。 */
int  hal_net_saved(net_saved_t * out, size_t cap);

/* 连接指定热点。PSK 传 NULL 或空串表示开放网络（key_mgmt NONE）。
 * ★ 返回 SAFE_OK 只代表「配置已下发给 wpa_supplicant」，此时握手多半还没完成；
 *   真正的连通要由调用方轮询 hal_net_status() 确认（这是无线链路的固有异步性，
 *   不是本实现偷懒——任何声称「返回值代表已连上」的封装都是假的）。 */
int  hal_net_connect(const char * ssid, const char * psk);

/* 断开并移除当前热点配置。后端不支持返回 SAFE_ERR_UNSUP。 */
int  hal_net_disconnect(void);

/* 供 net_service 后端注册使用（不是给业务层调的）。 */
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

