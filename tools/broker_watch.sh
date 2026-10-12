#!/bin/sh
# broker_watch —— 周期性扫描可用代理主机，结果原子写入地址文件。
#
# 为什么需要它（D38）：启动脚本只跑一次 find_broker，而板子 WiFi 要到 ~27s 才
# link ready，S05lvgl 几秒就起来了 —— 那一次扫描必然失败并回退到陈旧 hint 地址，
# 之后不再重试。本守护负责运行期持续订正，app 每次建连前读取（见
# mqtt_client.c: mqtt_refresh_host_from_file()），于是换网/换热点后无需重启。
#
# 写入用 tmp + rename 原子替换：与项目掉电安全同源约定，避免 app 读到半截内容。

FILE=${SAFE_BROKER_HOST_FILE:-/etc/safe/broker_host}
HINT=${SAFE_MQTT_HINT:-}
PERIOD=${SAFE_BROKER_WATCH_PERIOD:-30}

mkdir -p "$(dirname "$FILE")" 2>/dev/null

while :; do
    B=$(python3 /usr/share/lvgl/find_broker.py \
            --ca /etc/safe/certs/ca.crt \
            --cert /etc/safe/certs/device.crt \
            --key /etc/safe/certs/device.key \
            --port 8883 --hint "$HINT" 2>/dev/null | tail -1)

    if [ -n "$B" ]; then
        CUR=$(cat "$FILE" 2>/dev/null)
        if [ "$B" != "$CUR" ]; then
            echo "$B" > "$FILE.tmp" 2>/dev/null && mv "$FILE.tmp" "$FILE" 2>/dev/null
            echo "[broker_watch] $(date +%T) 代理主机 $CUR -> $B"
        fi
    fi
    sleep "$PERIOD"
done
