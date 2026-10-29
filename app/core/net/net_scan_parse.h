/**
 * @file net_scan_parse.h
 * 解析 wpa_supplicant 命令的返回文本（只做字符串解析，不碰 socket）。
 *
 * 单独拆出来是为了能单测：给一段固定文本，断言解析结果，
 * 不需要真网卡、真权限、真路由器。
 *
 * 三种输入格式：
 *   1) SCAN_RESULTS：每行 <bssid>\t<频率>\t<信号>\t<flags>\t<ssid>
 *      ★ SSID 是最后一个字段，**可能带空格**，所以必须按 tab 切，不能按空格切。
 *   2) STATUS：每行 key=value，例如 ssid=HomeWiFi。
 *   3) LIST_NETWORKS：每行 <id>\t<ssid>\t<bssid>\t<flags>，第一行是表头。
 *
 * 所有函数都不做内存分配；输入畸形时返回错误或截断，绝不越界、不崩溃。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 字段长度按协议实际上限留：
 * SSID 最大 32 字节；bssid 是 17 个字符的 MAC；flags 是拼接串，实测能超 40，给 64。 */
#define WPA_SSID_MAX   32
#define WPA_BSSID_MAX  17
#define WPA_FLAGS_MAX  64

typedef struct {
    char ssid[WPA_SSID_MAX + 1];
    char bssid[WPA_BSSID_MAX + 1];
    char flags[WPA_FLAGS_MAX + 1];
    int  freq_mhz;      /* 例如 2412 / 5180；解析失败为 0 */
    int  signal_dbm;    /* 负值，例如 -45；解析失败为 0 */
} wpa_ap_t;

/* LIST_NETWORKS 里的一条「已保存网络」。 */
typedef struct {
    int  id;                          /* wpa 的网络编号，后面删除 / 连接要用它 */
    char ssid[WPA_SSID_MAX + 1];      /* 可能为空串（只建了条目还没填名字） */
} net_saved_t;

/* 解析 SCAN_RESULTS。
 * 返回写入 out 的条数；超过 cap 的丢弃；字段不全的行整行跳过；
 * text 或 out 为 NULL 返回 0。
 * 空 SSID 也保留 —— 隐藏网络在业务上是有意义的，是否显示交给上层决定。 */
int wpa_parse_scan_results(const char * text, wpa_ap_t * out, size_t cap);

/* 在 STATUS 文本里按 key 取值。返回 0 = 找到；-4 = 没有这个键；-2 = 参数不对。 */
int wpa_status_get(const char * text, const char * key, char * out, size_t cap);

/* 解析 LIST_NETWORKS。返回写入 out 的条数；id 解析不出来的行跳过；
 * SSID 为空的行保留（上层要能看到并清理这种残留条目）。 */
int wpa_parse_list_networks(const char * text, net_saved_t * out, size_t cap);

#ifdef __cplusplus
}
#endif
