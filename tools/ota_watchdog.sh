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
    # 重启入口必须跟着「谁在管进程」走。
    # ★ 有 supervisor 时不能 killall：那会被判成一次崩溃、占用重启配额，
    #   而且 supervisor 会立刻把「旧二进制」再拉起来，刚回滚的版本反而没生效。
    #   交给 init 脚本 restart：先转发 TERM 让旧实例优雅退出，再重新拉起新文件。
    if [ -x /etc/init.d/S05safe ]; then
        /etc/init.d/S05safe restart
    else
        killall lvglsim 2>/dev/null
        rm -f /var/run/lvgl.pid
        /etc/init.d/S05lvgl start
    fi
done
