#!/bin/sh
# WiFi 开机自连（2026-10-02：韦东山流程 + wpa_cli -a DHCP 联动）
# 配置见 /etc/wpa_supplicant.conf（update_config=1 支持应用 SAVE_CONFIG 持久化）
# 前置：/etc/init.d/S45network-manager 已移至 /root/disabled_init/（防抢占 wlan0）
start() {
    ifconfig wlan0 up
    wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
    wpa_cli -a /etc/wpa_dhcp.sh -B
    echo "wifi: started"
}
stop() {
    killall wpa_cli 2>/dev/null
    killall wpa_supplicant 2>/dev/null
    killall udhcpc 2>/dev/null
    echo "wifi: stopped"
}
case "$1" in
    start|""|restart) stop; start ;;
    stop) stop ;;
    *) echo "Usage: $0 {start|stop|restart}"; exit 1 ;;
esac
