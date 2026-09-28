#!/bin/bash
# lab_cpu.sh —— MQTT 空转（忙轮询）判据：分别采「进程级」与「net 线程级」CPU
#
# 用法：bash tools/lab_cpu.sh [端口] [connack 码]
#   bash tools/lab_cpu.sh            # 默认 1893 / connack 5（永久错误场景）
#   bash tools/lab_cpu.sh 1893 3     # 瞬时错误场景（会持续退避重连）
#
# ---------------- 为什么要两个口径 ----------------
# ★ 这台机用 llvmpipe **软件渲染**，应用本身（UI 线程）就要 8~19% CPU：
#     基线：健康连接 18~19%   无 broker 12~15%
#   所以「进程级 16%」不等于有忙轮询，必须对照基线；而 net 线程单独看则
#   干净得多（修复前 100% → 修复后 0%）。
#   对外结论统一写成：
#     「进程级从 115% 降到 UI 基线 16%；net 线程单独看 100% → 0%」
#
# ---------------- 为什么必须采「稳态」 ----------------
# 只修一半时（关了 fd 但 poll 超时仍被压成 0），"刚出错那几秒"是 19%，
# 看起来像好转，**但稳态仍是 100%**。判据取瞬时值会放过一个 100% 的忙循环，
# 所以本脚本采两段：EARLY（刚出错）与 STEADY（稳定 30s 后），两段都要看。
#
# 时钟滴答口径：/proc/<pid>/stat 第 14/15 字段 = utime/stime，100Hz
#   → 5 秒内 500 滴答 = 100% 单核
set -u
PORT="${1:-1893}"
RC="${2:-5}"
APP_LOG=/tmp/lab_cpu_app.log
BRK_LOG=/tmp/lab_cpu_brk.log
mkdir -p /tmp/labdata

pgrep -x Xvfb >/dev/null 2>&1 || (Xvfb :99 -screen 0 800x480x24 >/dev/null 2>&1 &)
sleep 1
pkill -f "mqtt_lab.py broker" 2>/dev/null
pkill -x lvglsim 2>/dev/null
sleep 0.5

python3 tools/mqtt_lab.py broker --port "$PORT" --connack "$RC" --idle 180 \
    > "$BRK_LOG" 2>&1 &
sleep 1
SAFE_DATA_DIR=/tmp/labdata DISPLAY=:99 \
    SAFE_MQTT_HOST=127.0.0.1 SAFE_MQTT_PORT="$PORT" \
    timeout 120 stdbuf -oL -eL ./build_pc/bin/lvglsim > "$APP_LOG" 2>&1 &
APID=$(ps -eo pid,comm | awk '$2=="lvglsim"{print $1}' | head -1)
echo "APP_PID=$APID  端口=$PORT  CONNACK=$RC"

# 等 MQTT 真正起来再采样：应用启动有相机/串口重试，固定 sleep 会采到空窗口
for i in $(seq 1 60); do
    if grep -q "设备已连接" "$BRK_LOG" 2>/dev/null; then break; fi
    sleep 1
done

TID=$(ps -L -p "$APID" -o tid,comm | awk '$2=="safe-mqtt-net"{print $1}')
echo "net 线程 tid=${TID:-未找到}"

sample() {
    local label="$1"
    local pa pb pn ta tb tn
    pa=$(awk '{print $14+$15}' "/proc/$APID/stat")
    if [ -n "$TID" ]; then ta=$(awk '{print $14+$15}' "/proc/$APID/task/$TID/stat"); fi
    sleep 5
    pb=$(awk '{print $14+$15}' "/proc/$APID/stat")
    if [ -n "$TID" ]; then tb=$(awk '{print $14+$15}' "/proc/$APID/task/$TID/stat"); fi
    pn=$((pb - pa))
    echo "★ [$label] 进程级：5s 内 ${pn} 滴答 → $((pn * 100 / 500))% 单核"
    if [ -n "$TID" ]; then
        tn=$((tb - ta))
        echo "★ [$label] net线程：5s 内 ${tn} 滴答 → $((tn * 100 / 500))% 单核"
    fi
}

sample "EARLY 刚出错"
sleep 25
sample "STEADY 稳定30s后"

echo "--- ps -L（末次）---"
ps -L -p "$APID" -o tid,comm,pcpu | grep -E "TID|safe-"
echo "--- 设备侧 [MQTT] 日志 ---"
grep -aE "\[MQTT\]" "$APP_LOG" | tail -5
echo "--- broker 侧接受的连接数 ---"
grep -c "设备已连接" "$BRK_LOG"
pkill -f "mqtt_lab.py broker" 2>/dev/null
pkill -x lvglsim 2>/dev/null
echo "==== DONE ===="
