#!/bin/sh
# wpa_cli -a 事件联动（DHCP 由本脚本驱动，应用层不关心 IP）
# CONNECTED -> 重启 udhcpc 拿新网段租约；DISCONNECTED -> 清 IP
IFNAME=$1
ACTION=$2
case "$ACTION" in
    CONNECTED)
        killall udhcpc 2>/dev/null
        ip addr flush dev $IFNAME 2>/dev/null
        udhcpc -b -i $IFNAME
        ;;
    DISCONNECTED)
        killall udhcpc 2>/dev/null
        ip addr flush dev $IFNAME 2>/dev/null
        ;;
esac
exit 0
