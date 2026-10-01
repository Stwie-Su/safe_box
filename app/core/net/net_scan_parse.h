/**
 * @file net_scan_parse.h
 * wpa_supplicant ctrl_iface 应答的**纯解析**（无 IO、无堆分配、不取时间）。
 *
 * 为什么独立出来：它和 remote/mqtt_codec.c 是同一范式——把协议中对正确性
 * 要求最高的部分剥离成纯函数，于是能在 ctest 里用固定输入做确定性断言，
 * 不必依赖真网卡、真 wpa_supplicant 或真权限。HAL 层只负责把 socket 里的
 * 字节搬进来交给它。
 *
 * 涉及两条命令的输出格式（均以 \n 分行，\r\n 亦需容忍）：
 *   1) SCAN_RESULTS
 *        bssid / frequency / signal / flags / ssid
 *        <tab>分隔：<bssid>\t<freq>\t<signal>\t<flags>\t<ssid带空格>
 *      ★ SSID 是最后一个字段且允许含空格、tab 之后的全部内容都属于它，
 *        所以必须按 tab 切，绝不能按空格切（这是本模块最容易写错的地方）。
 *   2) STATUS
 *        <key>=<value>\n   形如 ssid=HomeWiFi / wpa_state=COMPLETED
 *
 * 目标5504行以内的正确性要求：任何畸形输入都不得越界写、不得崩溃，
 * 一律返回错误或截断后的安全结果。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 字段长度取「实际数据上限 + 1」，不是拍脑袋：
 *  - ssid   32+1（802.11 上限 32 字节）
 *  - bssid  "xx:xx:xx:xx:xx:xx" 17+1
 *  - flags  "[WPA2-PSK-CCMP][ESS]" 这类拼接，实测可超 40，给 64 */
#define WPA_SSID_MAX   32
#define WPA_BSSID_MAX  17
#define WPA_FLAGS_MAX  64

typedef struct {
    char ssid[WPA_SSID_MAX + 1];
    char bssid[WPA_BSSID_MAX + 1];
    char flags[WPA_FLAGS_MAX + 1];
    int  freq_mhz;      /* 例如 2412 / 5180；解析失败置 0 */
    int  signal_dbm;    /* 负值，例如 -45；解析失败置 0 */
} wpa_ap_t;

/* LIST_NETWORKS 里的一条已存网络。 */
typedef struct {
    int  id;                          /* wpa 的网络块编号，后续 REMOVE/SELECT 要用 */
    char ssid[WPA_SSID_MAX + 1];      /* 未设 SSID 或被转义时可能为空串 */
} net_saved_t;

/**
 * 解析 `SCAN_RESULTS` 的输出。
 *
 * @param text  wpa_supplicant 的原始应答（可为 NULL）
 * @param out   调用方提供的数组
 * @param cap   数组容量（可为 0）
 * @return 成功解析并写入的热点个数（0 表示没有可用行）：
 *         - 超过 cap 的行**被丢弃**（返回 cap），不写越界；
 *         - 单行字段不足或 freq/signal 非法，则该行整体跳过（不产出残缺记录）；
 *         - text 为 NULL 或 out 为 NULL 时返回 0。
 *
 * ★ 刻意不对 SSID 做「合法性」过滤：空 SSID 的隐藏网络在业务上有意义，
 *   是否显示由上层决定，解析层不擅自丢弃数据。
 */
int wpa_parse_scan_results(const char * text, wpa_ap_t * out, size_t cap);

/**
 * 在 `STATUS` 输出里按键取值。
 *
 * @return 0 找到；-4 未找到（key 不存在）；-2 参数非法。
 *         值超长时**截断**并返回 0（宁可截断也不报错能让上层拿到可读前缀，
 *         且与下面的扫描容错口径一致：畸形输入不出崩溃、不出空串）。
 */
int wpa_status_get(const char * text, const char * key, char * out, size_t cap);

/**
 * 解析 `LIST_NETWORKS` 的输出（已保存的网络块清单）。
 *
 * 行格式：<id>\t<ssid>\t<bssid>\t<flags>，首行为表头 "network id / ssid / ..."。
 *
 * @return 写入 out 的条数（超过 cap 的丢弃）；参数非法返回 0。
 *         id 解析失败的行跳过。SSID 为空串的行**保留**（wpa 里确实存在
 *         只建了块还没填 SSID 的记录，上层要给机会清理掉它）。
 */
int wpa_parse_list_networks(const char * text, net_saved_t * out, size_t cap);

#ifdef __cplusplus
}
#endif

