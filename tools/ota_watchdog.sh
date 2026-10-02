#!/bin/sh
# OTA 回滚看门狗：.ota_pending 在 45s 内未被应用清除 -> 新版本异常，回滚 .old
while true; do
    sleep 10
    M=/usr/share/lvgl/.ota_pending
    [ -f "$M" ] || continue
    TS=$(cat "$M" 2>/dev/null)
    NOW=$(date +%s)
    case "$TS" in ''|*[!0-9]*) continue;; esac
    [ $((NOW - TS)) -lt 45 ] && continue
    echo "$(date) watchdog: pending age $((NOW-TS))s, rolling back" >> /var/log/ota.log
    if [ -f /usr/share/lvgl/lvglsim.old ]; then
        mv -f /usr/share/lvgl/lvglsim.old /usr/share/lvgl/lvglsim
        [ -f /usr/share/lvgl/ota_version.old ] && mv -f /usr/share/lvgl/ota_version.old /usr/share/lvgl/ota_version
        echo "$(date) rollback done" >> /var/log/ota.log
    fi
    rm -f "$M"
    killall lvglsim 2>/dev/null
    rm -f /var/run/lvgl.pid
    /etc/init.d/S05lvgl start
done
