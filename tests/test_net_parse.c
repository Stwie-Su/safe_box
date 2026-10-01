/**
 * @file test_net_parse.c
 * WLAN 后端协议解析层单测（core/net/net_scan_parse）。
 *
 * 为什么这些用例值得写：解析层是"配置被写进 wpa_supplicant 之前唯一的把关点"。
 * 一条 SSID 里带空格的行被按空格切开，就会把 "My Home" 拆成两个字段，
 * 后续字段错位 → psk 被塞进 flags → 配到一个根本不存在的网络。
 * 这类 bug 在没有无线网卡的环境里跑 UI 也复现不出来，只能靠这里守住。
 *
 * 覆盖：正常多行 / SSID 含空格 / 表头跳过 / \r\n 容忍 / 容量截断 /
 *       字段缺失整行作废 / 数值非法作废 / 负 RSSI / 超长 SSID 截断 /
 *       STATUS 取值、前缀不误命中、键不存在、值截断、非法入参。
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "core/net/net_scan_parse.h"

/* ---------- 1. 基本扫描结果解析 ---------- */
static void t_scan_basic(void)
{
    const char * txt =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-45\t[WPA2-PSK-CCMP][ESS]\tHomeWiFi\n"
        "aa:bb:cc:dd:ee:ff\t5180\t-72\t[WPA-PSK-TKIP][ESS]\tChinaNet-abc123\n";

    wpa_ap_t ap[8];
    int n = wpa_parse_scan_results(txt, ap, 8);
    CHECK(n == 2);

    CHECK(strcmp(ap[0].ssid, "HomeWiFi") == 0);
    CHECK(strcmp(ap[0].bssid, "00:11:22:33:44:55") == 0);
    CHECK(strcmp(ap[0].flags, "[WPA2-PSK-CCMP][ESS]") == 0);
    CHECK(ap[0].freq_mhz == 2412);
    CHECK(ap[0].signal_dbm == -45);

    CHECK(strcmp(ap[1].ssid, "ChinaNet-abc123") == 0);
    CHECK(ap[1].freq_mhz == 5180);
    CHECK(ap[1].signal_dbm == -72);
}

/* ---------- 2. SSID 含空格（本模块最容易写错的一处） ---------- */
static void t_scan_ssid_with_space(void)
{
    const char * txt =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-50\t[WPA2-PSK-CCMP][ESS]\tMy Home WiFi 5G\n";

    wpa_ap_t ap[4];
    int n = wpa_parse_scan_results(txt, ap, 4);
    CHECK(n == 1);
    /* 必须完整取到最后一个 tab 之后的全部内容，而不是被空格截断成 "My" */
    CHECK(strcmp(ap[0].ssid, "My Home WiFi 5G") == 0);
}

/* ---------- 3. \r\n 容忍 ---------- */
static void t_scan_crlf(void)
{
    const char * txt =
        "bssid / frequency / signal / flags / ssid\r\n"
        "00:11:22:33:44:55\t2437\t-60\t[ESS]\tOpenNet\r\n";

    wpa_ap_t ap[4];
    int n = wpa_parse_scan_results(txt, ap, 4);
    CHECK(n == 1);
    /* \r 不得混进最后一个字段（SSID 是行内最后一段，最容易被污染） */
    CHECK(strcmp(ap[0].ssid, "OpenNet") == 0);
    CHECK(ap[0].signal_dbm == -60);
}

/* ---------- 4. 容量截断：不越界、返回值为 cap ---------- */
static void t_scan_cap(void)
{
    const char * txt =
        "bssid / frequency / signal / flags / ssid\n"
        "00:00:00:00:00:01\t2412\t-30\t[ESS]\tAP1\n"
        "00:00:00:00:00:02\t2412\t-31\t[ESS]\tAP2\n"
        "00:00:00:00:00:03\t2412\t-32\t[ESS]\tAP3\n";

    wpa_ap_t ap[2];
    int n = wpa_parse_scan_results(txt, ap, 2);
    CHECK(n == 2);   /* 多于 cap 的行被丢弃，绝不多写 */
    CHECK(strcmp(ap[0].ssid, "AP1") == 0);
    CHECK(strcmp(ap[1].ssid, "AP2") == 0);

    /* 零容量：不写任何东西，返回 0 而不是崩溃 */
    CHECK(wpa_parse_scan_results(txt, NULL, 0) == 0);
    CHECK(wpa_parse_scan_results(txt, ap, 0) == 0);
}

/* ---------- 5. 畸形行整行作废 ---------- */
static void t_scan_malformed(void)
{
    /* 少一个字段（只有 3 个 tab 后的内容不完整） */
    const char * short_line =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-45\t[WPA2-PSK-CCMP][ESS]\n";
    wpa_ap_t ap[4];
    CHECK(wpa_parse_scan_results(short_line, ap, 4) == 0);

    /* frequency 不是数字 */
    const char * bad_freq =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\tNOT_A_NUMBER\t-45\t[ESS]\tAP\n";
    CHECK(wpa_parse_scan_results(bad_freq, ap, 4) == 0);

    /* signal 缺负号写成了 "45x" */
    const char * bad_sig =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t45x\t[ESS]\tAP\n";
    CHECK(wpa_parse_scan_results(bad_sig, ap, 4) == 0);

    /* 只有负号没有数字 */
    const char * only_sign =
        "bssid / frequency / signal / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-\t[ESS]\tAP\n";
    CHECK(wpa_parse_scan_results(only_sign, ap, 4) == 0);

    /* 畸形行不影响后续正常行的产出 */
    const char * mixed =
        "bssid / frequency / signal / flags / ssid\n"
        "11:11:11:11:11:11\tabc\t-45\t[ESS]\tBad\n"
        "22:22:22:22:22:22\t2412\t-70\t[ESS]\tGood\n";
    CHECK(wpa_parse_scan_results(mixed, ap, 4) == 1);
    CHECK(strcmp(ap[0].ssid, "Good") == 0);
}

/* ---------- 6. 空输入与超长字段截断 ---------- */
static void t_scan_edge(void)
{
    wpa_ap_t ap[4];
    CHECK(wpa_parse_scan_results(NULL, ap, 4) == 0);
    CHECK(wpa_parse_scan_results("", ap, 4) == 0);
    CHECK(wpa_parse_scan_results("\n\n\n", ap, 4) == 0);
    /* 只有表头没有数据 */
    CHECK(wpa_parse_scan_results("bssid / frequency / signal / flags / ssid\n", ap, 4) == 0);

    /* 超长 SSID：必须截断到 32 且不越界 */
    char long_txt[256];
    const char * ultra = "0123456789abcdefghijklmnopqrstuvwxyzABCDEF";  /* 40 字节 */
    snprintf(long_txt, sizeof(long_txt),
             "bssid / frequency / signal / flags / ssid\n"
             "00:11:22:33:44:55\t2412\t-45\t[ESS]\t%s\n", ultra);
    CHECK(wpa_parse_scan_results(long_txt, ap, 4) == 1);
    CHECK(strlen(ap[0].ssid) == WPA_SSID_MAX);
    CHECK(strncmp(ap[0].ssid, ultra, WPA_SSID_MAX) == 0);

    /* 超长 flags 同样截断 */
    char long_flags[512];
    memset(long_flags, 'X', sizeof(long_flags) - 1);
    long_flags[sizeof(long_flags) - 1] = '\0';
    char long_txt2[1024];
    snprintf(long_txt2, sizeof(long_txt2),
             "bssid / frequency / signal / flags / ssid\n"
             "00:11:22:33:44:55\t2412\t-45\t%s\tAP\n", long_flags);
    CHECK(wpa_parse_scan_results(long_txt2, ap, 4) == 1);
    CHECK(strlen(ap[0].flags) == WPA_FLAGS_MAX);
}

/* ---------- 7. STATUS 键值取值 ---------- */
static void t_status_get(void)
{
    const char * st =
        "bssid=00:11:22:33:44:55\n"
        "freq=2412\n"
        "ssid=HomeWiFi\n"
        "id=0\n"
        "mode=station\n"
        "wpa_state=COMPLETED\n"
        "ip_address=192.168.1.42\n";

    char v[64];

    CHECK(wpa_status_get(st, "ssid", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "HomeWiFi") == 0);

    CHECK(wpa_status_get(st, "wpa_state", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "COMPLETED") == 0);

    CHECK(wpa_status_get(st, "ip_address", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "192.168.1.42") == 0);

    /* ★ 前缀不误命中：key="ssid" 不得被 "ssid_ext" / "bssid" 这类前缀行命中。
     * 注意 bssid= 位于首行，若用 strstr 实现就会把 bssid 的值错当成 ssid。 */
    const char * tricky =
        "bssid=aa:bb:cc:dd:ee:ff\n"
        "ssid_ext=something\n"
        "ssid=RealOne\n";
    CHECK(wpa_status_get(tricky, "ssid", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "RealOne") == 0);

    /* 键不存在 */
    CHECK(wpa_status_get(st, "no_such_key", v, sizeof(v)) == -4);

    /* 值超长：截断且返回成功（宁可拿前缀也不返回空，见头文件契约） */
    const char * longval = "key=0123456789abcdefghijklmnopqrstuvwxyz\n";
    char small[8];
    CHECK(wpa_status_get(longval, "key", small, sizeof(small)) == 0);
    CHECK(strlen(small) == 7);
    CHECK(strcmp(small, "0123456") == 0);

    /* cap==1：只能放下结束符 */
    char tiny[1];
    CHECK(wpa_status_get(longval, "key", tiny, sizeof(tiny)) == 0);
    CHECK(tiny[0] == '\0');

    /* 非法入参 */
    CHECK(wpa_status_get(NULL, "ssid", v, sizeof(v)) == -2);
    CHECK(wpa_status_get(st, NULL, v, sizeof(v)) == -2);
    CHECK(wpa_status_get(st, "", v, sizeof(v)) == -2);
    CHECK(wpa_status_get(st, "ssid", NULL, 8) == -2);
    CHECK(wpa_status_get(st, "ssid", v, 0) == -2);
}

/* ---------- 8. LIST_NETWORKS（已保存网络） ---------- */
static void t_list_networks(void)
{
    const char * txt =
        "network id / ssid / bssid / flags\n"
        "0\tHomeWiFi\tany\t[CURRENT]\n"
        "1\\tMy Home 5G\tany\t[DISABLED]\n"   /* 占位：下面用真样例覆盖 */
        ;
    (void)txt;

    const char * real =
        "network id / ssid / bssid / flags\n"
        "0\tHomeWiFi\tany\t[CURRENT]\n"
        "1\tOffice 5G\tany\t[DISABLED]\n"
        "2\tmy_hidden_net\tany\t[DISABLED]\n";

    net_saved_t sv[8];
    int n = wpa_parse_list_networks(real, sv, 8);
    CHECK(n == 3);
    CHECK(sv[0].id == 0);
    CHECK(strcmp(sv[0].ssid, "HomeWiFi") == 0);
    CHECK(sv[1].id == 1);
    CHECK(strcmp(sv[1].ssid, "Office 5G") == 0);   /* SSID 含空格 */
    CHECK(sv[2].id == 2);
    CHECK(strcmp(sv[2].ssid, "my_hidden_net") == 0);

    /* 空 SSID 的僵尸配置必须保留：上层要靠它清理 */
    const char * with_empty =
        "network id / ssid / bssid / flags\n"
        "3\t\tany\t[DISABLED]\n";
    CHECK(wpa_parse_list_networks(with_empty, sv, 8) == 1);
    CHECK(sv[0].id == 3);
    CHECK(sv[0].ssid[0] == '\0');

    /* id 非法的行跳过 */
    const char * bad_id =
        "network id / ssid / bssid / flags\n"
        "abc\tWhatever\tany\t[DISABLED]\n";
    CHECK(wpa_parse_list_networks(bad_id, sv, 8) == 0);

    /* 只有表头 */
    CHECK(wpa_parse_list_networks("network id / ssid / bssid / flags\n", sv, 8) == 0);

    /* 容量与非法入参 */
    CHECK(wpa_parse_list_networks(real, sv, 2) == 2);
    CHECK(wpa_parse_list_networks(NULL, sv, 8) == 0);
    CHECK(wpa_parse_list_networks(real, NULL, 0) == 0);
}

/* ---------- 9. \r\n 形式的 STATUS ---------- */
static void t_status_crlf(void)
{
    const char * st = "ssid=MyNet\r\nwpa_state=COMPLETED\r\n";
    char v[64];
    CHECK(wpa_status_get(st, "ssid", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "MyNet") == 0);   /* 值尾部不得带 \r */
    CHECK(wpa_status_get(st, "wpa_state", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "COMPLETED") == 0);
}

int main(void)
{
    t_scan_basic();
    t_scan_ssid_with_space();
    t_scan_crlf();
    t_scan_cap();
    t_scan_malformed();
    t_scan_edge();
    t_status_get();
    t_list_networks();
    t_status_crlf();
    return 0;
}

