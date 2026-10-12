#!/bin/sh
# LVGL 智能保险柜 启动包装（root 运行，见 /etc/init.d/S05lvgl）
export LV_LINUX_FBDEV_DEVICE=/dev/fb0
export LV_LINUX_EVDEV_POINTER_DEVICE=/dev/input/event1

# 触摸标定：默认恒等映射（上报值即屏幕像素）；旋转由 LVGL indev 核心自动变换
export LV_TOUCH_CAL_MINX=0
export LV_TOUCH_CAL_MINY=0
export LV_TOUCH_CAL_MAXX=1023
export LV_TOUCH_CAL_MAXY=599

# FM225 模组串口：FO101-KIT J5 USB -> CH340E -> /dev/ttyUSB0（ch341.ko 已固化）
export SAFE_FM225_DEV=/dev/ttyUSB0
# 屏幕倒装 180 度：fbdev 渲染旋转，触摸由 LVGL indev 核心自动变换
export SAFE_DISPLAY_ROT=180
# 性能探针：每 2s 打一行主循环耗时
export SAFE_PERF_LOG=1
export SAFE_TIME_BACKEND=rtc
export SAFE_RTC_DEV=/dev/rtc0

# 远程通道目标：默认先用「上次成功地址」，不通则由 find_broker 扫描本网段。
# ★ 为什么必须自愈：板子与代理主机都走热点/校园网 DHCP，地址会漂
#   （实测 .148.51 -> .163.202 -> .163.51）。写死 IP 时换网即断线且零报错，
#   表象像「MQTT 崩了」，实际只是连错机器（ping 通但 TCP refused）。
export SAFE_MQTT_HINT=10.123.24.51
BROKER=$(python3 /usr/share/lvgl/find_broker.py \
            --ca /etc/safe/certs/ca.crt \
            --cert /etc/safe/certs/device.crt \
            --key /etc/safe/certs/device.key \
            --port 8883 --hint "$SAFE_MQTT_HINT" 2>/dev/null)
if [ -n "$BROKER" ]; then
    export SAFE_MQTT_HOST="$BROKER"
    export SAFE_OTA_URL="http://$BROKER:8080"
    echo "[run] 发现代理主机 $BROKER"
else
    export SAFE_MQTT_HOST="$SAFE_MQTT_HINT"
    export SAFE_OTA_URL="http://$SAFE_MQTT_HINT:8080"
    echo "[run] 未发现代理主机，回退 $SAFE_MQTT_HINT"
fi

printf '\033[9;0]' > /dev/tty1 2>/dev/null

# insmod ch341（已加载时静默失败，无害）
insmod /lib/modules/4.9.88/extra/ch341.ko 2>/dev/null

# broker 地址守护：后台周期性订正代理主机（换网自愈，见 D38）
if ! kill -0 "$(cat /var/run/broker_wd.pid 2>/dev/null)" 2>/dev/null; then
    setsid /usr/share/lvgl/broker_watch.sh >> /tmp/broker_watch.log 2>&1 &
    echo $! > /var/run/broker_wd.pid
fi

# OTA 健康标记 + 回滚看门狗：应用 20s 后清标记；45s 未清 -> 看门狗回滚 .old
date +%s > /usr/share/lvgl/.ota_pending
if ! kill -0 "$(cat /var/run/ota_wd.pid 2>/dev/null)" 2>/dev/null; then
    setsid /usr/share/lvgl/ota_watchdog.sh >/dev/null 2>&1 &
    echo $! > /var/run/ota_wd.pid
fi

cd /usr/share/lvgl || exit 1

# stdbuf 行缓冲：让 [fm225]/[CAMERA] 等 printf 即时落盘
exec stdbuf -oL -eL ./lvglsim > /var/log/lvglsim.log 2>&1

