#!/bin/bash
# lab_reconnect.sh —— MQTT 重连频率判据：数 N 秒内重连了几次，并打印退避曲线
#
# 用法：bash tools/lab_reconnect.sh [端口] [窗口秒数] [broker 额外参数...]
#   bash tools/lab_reconnect.sh                       # 默认 1894 / 30s / --suback-rc 128
#   bash tools/lab_reconnect.sh 1894 30 --suback-rc 128   # SUBACK 被拒（缺陷 #11）
#   bash tools/lab_reconnect.sh 1893 30 --connack 3       # CONNACK 瞬时错误（server unavailable）
#   bash tools/lab_reconnect.sh 1895 30 --no-suback       # 不回 SUBACK（缺陷 #12）
#
# ---------------- 为什么要数「次数」而不是看单条日志 ----------------
# 退避有没有生效，单看一条日志是看不出来的：每次重连的日志都长得一模一样
# （CONNECT → CONNACK → SUBSCRIBE → 断链）。差别只在**频率** ——
# 退避被意外清零时，设备会以 0.5s 间隔疯狂重连（30s 内 ~70 次）；
# 退避正常爬升到 30s 封顶时，30s 内只有 ~7 次。
# 所以判据必须是「固定窗口内的重连次数」，而且要给出间隔序列（退避曲线），
# 否则「重连了 7 次」到底是 0.5,0.5,0.5 还是 0.5,1,2,4,8,16,30 无从分辨。
#
# ---------------- 为什么给 broker 日志加时间戳 ----------------
# mqtt_lab.py 的 log() 不打印时间戳（保持输出干净，便于逐字节核对报文）。
# 但退避曲线需要精确间隔，所以本脚本在管道里逐行补一个 epoch 毫秒前缀，
# 再由 awk 对相邻两次「设备已连接」求差。这样既不改工具，又拿到毫秒级数据。
#
# ---------------- 判据（本机实测）----------------
#   SUBACK 被拒场景：修复前 30s 内 ~70 次（CONNACK(0) 把退避清零）
#                    修复后 30s 内 7 次（suback_reject_streak 独立退避，爬到 30s 封顶）
#   期望间隔序列：0.5 1 2 4 8 16 30 30 30 …（封顶 30000ms）
#
# 时钟滴答口径见 tools/lab_cpu.sh；本脚本只看「次数 + 间隔」，不看 CPU。
set -u
PORT="${1:-1894}"
WIN="${2:-30}"
shift 2 2>/dev/null || true
EXTRA="$*"
[ -z "$EXTRA" ] && EXTRA="--suback-rc 128"

APP_LOG=/tmp/lab_reconn_app.log
BRK_LOG=/tmp/lab_reconn_brk.log
mkdir -p /tmp/labdata

pgrep -x Xvfb >/dev/null 2>&1 || (Xvfb :99 -screen 0 800x480x24 >/dev/null 2>&1 &)
sleep 1
pkill -f "mqtt_lab.py broker" 2>/dev/null
pkill -x lvglsim 2>/dev/null
sleep 0.5

# 逐行补 epoch 毫秒前缀（见文件头「为什么给 broker 日志加时间戳」）
python3 tools/mqtt_lab.py broker --port "$PORT" $EXTRA --idle 180 2>&1 \
  | while IFS= read -r line; do printf '%s %s\n' "$(date +%s.%3N)" "$line"; done \
  > "$BRK_LOG" &
sleep 1
SAFE_DATA_DIR=/tmp/labdata DISPLAY=:99 \
    SAFE_MQTT_HOST=127.0.0.1 SAFE_MQTT_PORT="$PORT" \
    timeout $((WIN + 90)) stdbuf -oL -eL ./build_pc/bin/lvglsim > "$APP_LOG" 2>&1 &

echo "端口=$PORT  窗口=${WIN}s  broker 参数：$EXTRA"
# 等第一次连上再开始计窗口：应用启动阶段本身有相机/串口重试，不算 MQTT 重连
for i in $(seq 1 60); do
    if grep -q "设备已连接" "$BRK_LOG" 2>/dev/null; then break; fi
    sleep 1
done
START=$(date +%s.%3N)
echo "窗口起点 epoch=$START"

echo "……采样中（${WIN}s）……"
sleep "$WIN"
END=$(date +%s.%3N)

echo "=== 重连次数 ==="
awk -v s="$START" -v e="$END" '
    /设备已连接/ {
        if ($1+0 >= s+0 && $1+0 <= e+0) { n++; t[n] = $1+0 }
    }
    END {
        printf "窗口内连接次数：%d 次 / %.1fs\n", n, (e-s)
        if (n >= 2) {
            printf "退避曲线（相邻连接间隔，秒）："
            for (i = 2; i <= n; i++) printf " %.2f", t[i] - t[i-1]
            printf "\n"
        }
    }' "$BRK_LOG"

echo "=== 设备侧 [MQTT] 日志（末 5 条）==="
grep -aE "\[MQTT\]" "$APP_LOG" | tail -5

pkill -f "mqtt_lab.py broker" 2>/dev/null
pkill -x lvglsim 2>/dev/null
echo "==== DONE ===="
