#!/bin/sh
# ============================================================
#  板子启动入口：把应用交给 supervisor 守护（生产运行方式）
#
#  链路：
#    /etc/init.d/S05safe start
#      → 本脚本（一次性环境准备：fb/触摸/串口/RTC/broker 发现）
#      → exec safe_supervisor --daemon
#          → fork/exec lvglsim（崩溃重启、限速、安全态、看门狗接管）
#
#  ★ 为什么环境准备不放在被守护的应用里
#     broker 扫描要几秒、OTA 标记每次启动都写会让「OTA 回滚看门狗」误判；
#     这些只该在**整机启动**时做一次。应用重启时直接复用已 export 的环境。
#
#  ★ 互斥关系（不要叠加）：
#      supervisor 自己负责「崩溃重启 + 限速 + 安全态」。
#      若再用 inittab respawn 复活 supervisor，安全态退出（rc=2）会被 init
#      立刻重新拉起 → 应用又被启动 → 安全态被绕过。
#      二选一：S05 一次性启动本脚本，或 inittab respawn 直接跑应用（不用 supervisor）。
#
#  可用环境变量覆盖：
#      SAFE_APP            应用路径（默认 /usr/share/lvgl/lvglsim）
#      SAFE_SUPERVISOR     supervisor 路径（默认同目录 safe_supervisor）
#      SAFE_MAX_RESTARTS   滑动窗口内允许的重启次数（默认 5）
#      SAFE_WINDOW         滑动窗口秒数（默认 60）
#      SAFE_GRACE          优雅停机宽限秒数（默认 5）
#      SAFE_SAFE_CMD       安全态动作（默认 /etc/safe/safe_state.sh）
#      SAFE_SUP_LOG        日志路径（默认 /var/log/safe_supervisor.log）
#      SAFE_SUP_STATUS     状态文件（默认 /var/run/safe_supervisor.status）
#      SAFE_SUP_PID        锁/PID 文件（默认 /var/run/safe_supervisor.pid）
#      SAFE_WATCHDOG_DEV   看门狗设备（默认 /dev/watchdog；none=不接管）
# ============================================================
set -u

APP="${SAFE_APP:-/usr/share/lvgl/lvglsim}"
SUP="${SAFE_SUPERVISOR:-/usr/share/lvgl/safe_supervisor}"
LOG="${SAFE_SUP_LOG:-/var/log/safe_supervisor.log}"
STATUS="${SAFE_SUP_STATUS:-/var/run/safe_supervisor.status}"
PIDFILE="${SAFE_SUP_PID:-/var/run/safe_supervisor.pid}"

if [ ! -x "$SUP" ]; then
    echo "[safe_boot] supervisor 不存在或不可执行: $SUP —— 回退直接启动应用" >&2
    exec "$APP"
fi

# 运行目录：/var/run 可能是 tmpfs，重启后为空；/var/lib/safe 是持久数据。
mkdir -p /var/run /var/log /var/lib/safe

# ---- 显示与触摸（与 board_run_lvglsim.sh 保持一致）----
export LV_LINUX_FBDEV_DEVICE=/dev/fb0
export LV_LINUX_EVDEV_POINTER_DEVICE=/dev/input/event1
export LV_TOUCH_CAL_MINX=0
export LV_TOUCH_CAL_MINY=0
export LV_TOUCH_CAL_MAXX=1023
export LV_TOUCH_CAL_MAXY=599

# ---- 外设 ----
export SAFE_FM225_DEV=/dev/ttyUSB0
export SAFE_DISPLAY_ROT=180
export SAFE_PERF_LOG=1
export SAFE_TIME_BACKEND=rtc
export SAFE_RTC_DEV=/dev/rtc0
insmod /lib/modules/4.9.88/extra/ch341.ko 2>/dev/null

# ---- 看门狗（两层，板上 2026-10-19 实测确认）----
# 第一层 硬件看门狗（真狗）：i.MX6ULL 片内 WDOG，/dev/watchdog 存在，超时 60s。
#   应用主循环喂狗；进程被冻住/卡死 → 60s 后整机复位。代价是重启整机，
#   所以「快速卡死」交给第二层先手处理，真狗只兜底整机级异常。
# 第二层 软狗：应用主循环把单调时钟值写进心跳文件 → supervisor 读数值比较，
#   超过 --heartbeat-timeout（5s）判定卡死 → SIGTERM → 重启应用（保留现场、只重启进程）。
#   （用单调时钟而不是文件时间：这台设备有 RTC 校时和远程 sync_time，拨时钟会绕过判定。）
# 不显式设 SAFE_WATCHDOG_BACKEND，让 HAL 自动降级：linux → file → null。
#   真狗打不开的板子会自动退到软狗，不用改这里。
export SAFE_WATCHDOG_FILE="${SAFE_WATCHDOG_FILE:-/var/run/safe_heartbeat}"

# ---- 远程通道目标：先试上次成功地址，不通则扫描本网段 ----
# 板子与代理主机都走 DHCP，地址会漂；写死 IP 时换网即断线且零报错。
export SAFE_MQTT_HINT="${SAFE_MQTT_HINT:-10.123.24.51}"
BROKER=$(python3 /usr/share/lvgl/find_broker.py \
            --ca /etc/safe/certs/ca.crt \
            --cert /etc/safe/certs/device.crt \
            --key /etc/safe/certs/device.key \
            --port 8883 --hint "$SAFE_MQTT_HINT" 2>/dev/null)
if [ -n "$BROKER" ]; then
    export SAFE_MQTT_HOST="$BROKER"
    export SAFE_OTA_URL="http://$BROKER:8080"
    echo "[safe_boot] 发现代理主机 $BROKER"
else
    export SAFE_MQTT_HOST="$SAFE_MQTT_HINT"
    export SAFE_OTA_URL="http://$SAFE_MQTT_HINT:8080"
    echo "[safe_boot] 未发现代理主机，回退 $SAFE_MQTT_HINT"
fi

printf '\033[9;0]' > /dev/tty1 2>/dev/null

# 关掉开机画面。psplash 由 /etc/init.d/rcS 在最早拉起，原本由厂家 demo 脚本
# （S99myirhmi2）里的 psplash-write QUIT 收尾。我们不再启动那个 demo，
# 就得自己发 QUIT —— 否则 psplash 一直活着占着 /dev/fb0（另一个抢屏写者）。
if pidof psplash >/dev/null 2>&1; then
    /usr/bin/psplash-write "QUIT" 2>/dev/null
fi

# ---- OTA 健康标记 + 回滚看门狗（只在整机启动时写一次）----
date +%s > /usr/share/lvgl/.ota_pending
if ! kill -0 "$(cat /var/run/ota_wd.pid 2>/dev/null)" 2>/dev/null; then
    setsid /usr/share/lvgl/ota_watchdog.sh >/dev/null 2>&1 &
    echo $! > /var/run/ota_wd.pid
fi

# ---- broker 地址守护（换网自愈，见 D38）----
if ! kill -0 "$(cat /var/run/broker_wd.pid 2>/dev/null)" 2>/dev/null; then
    setsid /usr/share/lvgl/broker_watch.sh >> /tmp/broker_watch.log 2>&1 &
    echo $! > /var/run/broker_wd.pid
fi

cd /usr/share/lvgl || exit 1

# stdbuf 行缓冲：让 [fm225]/[CAMERA] 等 printf 即时落盘。
# ★ --daemon 后 stdio 指向 /dev/null，应用日志必须自己重定向到文件：
#   这里用 sh -c 包一层，把应用 stdout/stderr 追加到 /var/log/lvglsim.log。
exec "$SUP" --app "$APP" --daemon \
    --max-restarts "${SAFE_MAX_RESTARTS:-5}" \
    --window "${SAFE_WINDOW:-60}" \
    --grace "${SAFE_GRACE:-5}" \
    --safe-cmd "${SAFE_SAFE_CMD:-/etc/safe/safe_state.sh}" \
    --watchdog "${SAFE_WATCHDOG_DEV:-/dev/watchdog}" \
    --log "$LOG" --log-max "${SAFE_SUP_LOG_MAX:-1048576}" \
    --app-log "${SAFE_APP_LOG:-/var/log/lvglsim.log}" \
    --heartbeat "${SAFE_WATCHDOG_FILE:-/var/run/safe_heartbeat}" \
    --heartbeat-timeout "${SAFE_HB_TIMEOUT:-5}" \
    --heartbeat-startup "${SAFE_HB_STARTUP:-30}" \
    --status "$STATUS" --pidfile "$PIDFILE"
