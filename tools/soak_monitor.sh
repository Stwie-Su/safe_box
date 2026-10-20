#!/bin/sh
# ============================================================
#  长跑观测（72h）：每 5 分钟记一行，用来回答「跑久了会不会烂」
#
#  为什么必须单独跑：72h 里要暴露的是**缓慢型**问题 —— fd 泄漏、内存增长、
#  看门狗误触发、日志轮转是否真的滚。这些在几分钟的验收里看不出来，
#  只能靠连续的定长样本事后比对。
#
#  判据（每行都要能回答一个问题）：
#    rss       —— 应用常驻内存（看有没有单调上涨）
#    fds       —— 进程打开的 fd 数（看泄漏）
#    threads   —— 线程数（看 face/worker 有没有反复起）
#    restarts  —— supervisor 记的重启次数（看有没有误杀）
#    hb_age    —— 心跳距今多少毫秒（看软狗是否一直被喂）
#    avail     —— 根分区可用 KB（看日志是否撑爆 flash）
#
#  用法：setsid /usr/share/lvgl/soak_monitor.sh >/dev/null 2>&1 &
#        tail -f /var/log/soak.log
# ============================================================
LOG=${SAFE_SOAK_LOG:-/var/log/soak.log}
INTERVAL=${SAFE_SOAK_INTERVAL:-300}

[ -f "$LOG" ] || echo "# ts uptime rss_kb fds threads restarts hb_age_ms avail_kb" > "$LOG"

while :; do
    P=$(pidof lvglsim 2>/dev/null | awk '{print $1}')
    if [ -z "$P" ]; then
        echo "$(date '+%F %T') 0 - - - - - -" >> "$LOG"
        sleep "$INTERVAL"
        continue
    fi

    RSS=$(awk '/VmRSS/{print $2}' /proc/$P/status 2>/dev/null)
    FDS=$(ls /proc/$P/fd 2>/dev/null | wc -l)
    THR=$(awk '/Threads/{print $2}' /proc/$P/status 2>/dev/null)
    UPT=$(cut -d. -f1 /proc/uptime)
    RESTARTS=$(awk -F= '/^restarts=/{print $2}' /var/run/safe_supervisor.status 2>/dev/null)
    BEAT=$(cat /var/run/safe_heartbeat 2>/dev/null)
    case "$BEAT" in
        ''|*[!0-9]*) HBA="?" ;;
        *) HBA=$(( $(awk '{printf "%d", $1*1000}' /proc/uptime) - BEAT )) ;;
    esac
    AVAIL=$(df / | awk 'NR==2{print $4}')

    echo "$(date '+%F %T') $UPT ${RSS:-?} ${FDS:-?} ${THR:-?} ${RESTARTS:-?} ${HBA:-?} ${AVAIL:-?}" >> "$LOG"
    sleep "$INTERVAL"
done
