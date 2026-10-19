#!/usr/bin/env bash
# ============================================================
#  supervisor 行为测试（ctest）
#
#  覆盖的不是「函数返回值」，而是进程级契约：
#    正常退出不重启 / 崩溃重启 / 超限进安全态 / SIGTERM 转发 /
#    单实例锁 / daemon 模式 / 参数校验。
#
#  为什么用脚本而不是 C 单测：supervisor 的价值全在 fork/exec/waitpid/
#  signal 这些内核语义上，只有真起进程才能验证；mock 掉就什么都没测到。
# ============================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/supervisor/safe_supervisor.c"
CC="${CC:-cc}"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "[SKIP] 找不到 C 编译器（$CC）"
    exit 77
fi

TMP="$(mktemp -d /tmp/safe_sup_test.XXXXXX)"
cleanup() {
    # 先 TERM 再 KILL：daemon 用例会留下后台 supervisor
    for f in "$TMP"/*.pid; do
        [ -f "$f" ] || continue
        p="$(cat "$f" 2>/dev/null)"
        case "$p" in ''|*[!0-9]*) continue;; esac
        kill -TERM "$p" 2>/dev/null
    done
    sleep 0.3
    for f in "$TMP"/*.pid; do
        [ -f "$f" ] || continue
        p="$(cat "$f" 2>/dev/null)"
        case "$p" in ''|*[!0-9]*) continue;; esac
        kill -KILL "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

fails=0
check() {
    if ! eval "$2"; then
        echo "  FAIL: $1"
        fails=$((fails + 1))
    fi
}

echo "== 编译 supervisor =="
if ! "$CC" -O2 -Wall -Wextra -Werror -o "$TMP/sup" "$SRC"; then
    echo "[FAIL] supervisor 编译失败"
    exit 1
fi
echo "  [OK] $("$TMP/sup" --help 2>&1 | head -1)"

# 假应用：按第一个参数决定行为。
cat > "$TMP/app.sh" <<'APP'
#!/bin/sh
case "${1:-ok}" in
    ok)    exit 0 ;;
    crash) exit 7 ;;
    segv)  kill -SEGV $$ ;;
    hang)  trap 'echo term > "$2"; exit 0' TERM; while :; do sleep 1; done ;;
    slow)  trap 'sleep 1.5; echo done > "$2"; exit 0' TERM; while :; do sleep 1; done ;;
    args)  echo "$@" > "$2"; exit 0 ;;
    *)     exit 0 ;;
esac
APP
chmod +x "$TMP/app.sh"

echo "== 用例 1：正常退出不重启 =="
"$TMP/sup" --app "$TMP/app.sh" --status "$TMP/s1.status" \
    --pidfile "$TMP/s1.pid" --watchdog none -- ok >"$TMP/s1.log" 2>&1
rc=$?
check "退出码应为 0（实际 $rc）" "[ $rc -eq 0 ]"
check "状态应为 stopped" "grep -q '^state=stopped' '$TMP/s1.status'"
check "不应发生重启" "grep -q '^restarts=0' '$TMP/s1.status'"
check "日志应写明正常退出" "grep -q '正常退出' '$TMP/s1.log'"

echo "== 用例 2：崩溃重启 + 超限进安全态 =="
"$TMP/sup" --app "$TMP/app.sh" --max-restarts 2 --window 5 \
    --restart-delay 20 --grace 1 --watchdog none \
    --status "$TMP/s2.status" --pidfile "$TMP/s2.pid" \
    --log "$TMP/s2.log" -- crash >/dev/null 2>&1
rc=$?
check "退出码应为 2（实际 $rc）" "[ $rc -eq 2 ]"
check "状态应为 safe" "grep -q '^state=safe' '$TMP/s2.status'"
check "应记录 3 次重启（实际 $(grep '^restarts=' "$TMP/s2.status" 2>/dev/null)）" \
      "grep -q '^restarts=3' '$TMP/s2.status'"
check "日志应出现 SAFE-STATE" "grep -q 'SAFE-STATE' '$TMP/s2.log'"
check "日志应区分异常退出" "grep -q '异常退出 code=7' '$TMP/s2.log'"

echo "== 用例 3：SIGTERM 转发，子进程优雅退出 =="
"$TMP/sup" --app "$TMP/app.sh" --grace 2 --watchdog none \
    --status "$TMP/s3.status" --pidfile "$TMP/s3.pid" \
    --log "$TMP/s3.log" -- hang "$TMP/s3.term" >/dev/null 2>&1 &
SUP_PID=$!
sleep 0.5
kill -TERM "$SUP_PID" 2>/dev/null
wait "$SUP_PID"
rc=$?
check "退出码应为 0（实际 $rc）" "[ $rc -eq 0 ]"
check "子进程应真正收到 SIGTERM（trap 落文件）" "[ -f '$TMP/s3.term' ]"
check "状态应为 stopped" "grep -q '^state=stopped' '$TMP/s3.status'"

echo "== 用例 4：单实例锁 =="
"$TMP/sup" --app "$TMP/app.sh" --grace 1 --watchdog none \
    --pidfile "$TMP/s4.pid" --log "$TMP/s4.log" -- hang >/dev/null 2>&1 &
SUP_PID=$!
sleep 0.4
"$TMP/sup" --app "$TMP/app.sh" --watchdog none --pidfile "$TMP/s4.pid" \
    -- log >/dev/null 2>&1
rc=$?
check "第二个实例应返回 3（实际 $rc）" "[ $rc -eq 3 ]"
kill -TERM "$SUP_PID" 2>/dev/null
wait "$SUP_PID" 2>/dev/null

echo "== 用例 5：应用参数原样传递 =="
"$TMP/sup" --app "$TMP/app.sh" --watchdog none --pidfile "$TMP/s5.pid" \
    -- args "$TMP/s5.args" --flag value >/dev/null 2>&1
check "参数应原样传给应用" "grep -q -- '--flag value' '$TMP/s5.args'"

echo "== 用例 6：daemon 模式 =="
"$TMP/sup" --app "$TMP/app.sh" --daemon --watchdog none \
    --pidfile "$TMP/s6.pid" --status "$TMP/s6.status" --log "$TMP/s6.log" -- hang
rc=$?
check "父进程应立刻返回 0（实际 $rc）" "[ $rc -eq 0 ]"
sleep 0.6
check "pidfile 应存在" "[ -s '$TMP/s6.pid' ]"
check "daemon 应处于 running" "grep -q '^state=running' '$TMP/s6.status'"
DPID="$(cat "$TMP/s6.pid" 2>/dev/null)"
check "daemon 进程应存活" "kill -0 '$DPID' 2>/dev/null"
kill -TERM "$DPID" 2>/dev/null
sleep 0.6
check "daemon 应已退出" "! kill -0 '$DPID' 2>/dev/null"

echo "== 用例 7：参数校验 =="
"$TMP/sup" --app /no/such/app --watchdog none --pidfile "$TMP/s7.pid" >/dev/null 2>&1
check "不存在的应用应返回 1（实际 $?）" "[ $? -eq 1 ]"
"$TMP/sup" --watchdog none --pidfile "$TMP/s7.pid" >/dev/null 2>&1
check "缺少 --app 应返回 1（实际 $?）" "[ $? -eq 1 ]"

echo "== 用例 8：日志轮转 =="
: > "$TMP/s8.log"
"$TMP/sup" --app "$TMP/app.sh" --max-restarts 2 --window 5 --restart-delay 20 \
    --watchdog none --pidfile "$TMP/s8.pid" --log "$TMP/s8.log" --log-max 200 -- crash >/dev/null 2>&1
check "应生成轮转文件 .1" "[ -f '$TMP/s8.log.1' ]"

echo "== 用例 9：软狗——心跳过期判定卡死（超上限进安全态）=="
# 假应用：写一次很旧的心跳值（相当于启动后 1 秒写了一次），随后卡住不再更新
cat > "$TMP/hb_stale.sh" <<'APP'
#!/bin/sh
echo 1000 > "$1"
exec sleep 60
APP
chmod +x "$TMP/hb_stale.sh"
"$TMP/sup" --app "$TMP/hb_stale.sh" --max-restarts 0 --window 5 --restart-delay 20 \
    --grace 1 --watchdog none \
    --heartbeat "$TMP/hb_stale_beat" --heartbeat-timeout 1 --heartbeat-startup 1 \
    --status "$TMP/s9.status" --pidfile "$TMP/s9.pid" \
    --log "$TMP/s9.log" -- "$TMP/hb_stale_beat" >/dev/null 2>&1
rc=$?
check "退出码应为 2（实际 $rc）" "[ $rc -eq 2 ]"
check "日志应出现心跳超时" "grep -q '心跳超时' '$TMP/s9.log'"
check "状态应为 safe" "grep -q '^state=safe' '$TMP/s9.status'"

echo "== 用例 10：软狗不误杀持续写心跳的应用 =="
cat > "$TMP/hb_live.py" <<'PY'
#!/usr/bin/env python3
import sys, time
while True:
    with open(sys.argv[1], "w") as f:
        f.write(str(int(time.monotonic() * 1000)))
    time.sleep(0.2)
PY
chmod +x "$TMP/hb_live.py"
"$TMP/sup" --app "$TMP/hb_live.py" --watchdog none \
    --heartbeat "$TMP/hb_ok" --heartbeat-timeout 1 --heartbeat-startup 2 \
    --pidfile "$TMP/s10.pid" --log "$TMP/s10.log" -- "$TMP/hb_ok" >/dev/null 2>&1 &
SUP_PID=$!
sleep 2.5
check "持续写心跳时不应判定超时" "! grep -q '心跳超时' '$TMP/s10.log'"
check "supervisor 应仍在运行" "kill -0 $SUP_PID 2>/dev/null"
kill -TERM "$SUP_PID" 2>/dev/null
wait "$SUP_PID" 2>/dev/null

echo "== 用例 11：真狗场景下没有心跳文件，不应误杀 =="
"$TMP/sup" --app "$TMP/app.sh" --watchdog none \
    --heartbeat "$TMP/never_written" --heartbeat-timeout 1 --heartbeat-startup 1 \
    --pidfile "$TMP/s11.pid" --log "$TMP/s11.log" -- hang "$TMP/s11.term" >/dev/null 2>&1 &
SUP_PID=$!
sleep 3.5
check "没有心跳文件时不应判死" "! grep -q '心跳超时' '$TMP/s11.log'"
check "应给出提示而不是误杀" "grep -q '未检测到心跳文件' '$TMP/s11.log'"
check "supervisor 应仍在运行" "kill -0 $SUP_PID 2>/dev/null"
kill -TERM "$SUP_PID" 2>/dev/null
wait "$SUP_PID" 2>/dev/null

echo "== 用例 12：宽限期从「收到停机信号」起算，不是从应用启动起算 =="
# 背景：曾用「子进程已运行时长」判断宽限，长跑应用（本例跑 6s > grace 3s）
#       一收到停机信号就被立刻 SIGKILL，等于没有宽限，日志/存储来不及落盘（D46）。
"$TMP/sup" --app "$TMP/app.sh" --grace 3 --watchdog none \
    --pidfile "$TMP/s12.pid" --log "$TMP/s12.log" -- slow "$TMP/s12.done" >/dev/null 2>&1 &
SUP_PID=$!
sleep 6                     # 让应用运行时长明显超过 grace(3s)
kill -TERM "$SUP_PID" 2>/dev/null
wait "$SUP_PID"
rc=$?
check "退出码应为 0（实际 $rc）" "[ $rc -eq 0 ]"
check "应给足宽限让应用收尾（应出现 s12.done）" "[ -f '$TMP/s12.done' ]"
check "不应出现宽限超时 SIGKILL" "! grep -q 'SIGKILL' '$TMP/s12.log'"
check "应判定为正常退出" "grep -q '正常退出' '$TMP/s12.log'"

echo "== 用例 13：daemon 模式重复启动返回 3 =="
# 背景：抢锁曾在 daemonize 之后，父进程先 _exit(0)，重复启动被静默吞掉（D47）。
"$TMP/sup" --app "$TMP/app.sh" --daemon --watchdog none \
    --pidfile "$TMP/s13.pid" --status "$TMP/s13.status" \
    --log "$TMP/s13.log" -- hang >/dev/null 2>&1
sleep 0.8
"$TMP/sup" --app "$TMP/app.sh" --daemon --watchdog none \
    --pidfile "$TMP/s13.pid" --log "$TMP/s13b.log" -- hang >/dev/null 2>&1
rc=$?
check "第二个 daemon 实例应返回 3（实际 $rc）" "[ $rc -eq 3 ]"
D13="$(cat "$TMP/s13.pid" 2>/dev/null)"
case "$D13" in ''|*[!0-9]*) ;; *) kill -TERM "$D13" 2>/dev/null ;; esac
sleep 0.6

echo "== 用例 14：见过心跳之后卡死，不能被启动宽限拖住 =="
# 背景：启动宽限曾无条件生效，应用先写过心跳再卡死也要等满 30s 才判死（D48）。
cat > "$TMP/hb_then_stall.py" <<'PY'
#!/usr/bin/env python3
import sys, time
# 只写一次（新值），随后卡住不再更新 —— 模拟「主循环跑起来后死锁」
with open(sys.argv[1], "w") as f:
    f.write(str(int(time.monotonic() * 1000)))
time.sleep(3600)
PY
chmod +x "$TMP/hb_then_stall.py"
T0=$(date +%s)
"$TMP/sup" --app "$TMP/hb_then_stall.py" --max-restarts 0 --window 5 --restart-delay 20 \
    --grace 1 --watchdog none \
    --heartbeat "$TMP/hb_seen" --heartbeat-timeout 1 --heartbeat-startup 30 \
    --status "$TMP/s14.status" --pidfile "$TMP/s14.pid" \
    --log "$TMP/s14.log" -- "$TMP/hb_seen" >/dev/null 2>&1
rc=$?
T1=$(date +%s)
elapsed=$((T1 - T0))
check "应在 5s 内判死，而不是等满 30s 启动宽限（实际 ${elapsed}s）" "[ $elapsed -lt 5 ]"
check "日志应出现心跳超时" "grep -q '心跳超时' '$TMP/s14.log'"
check "退出码应为 2（安全态，实际 $rc）" "[ $rc -eq 2 ]"

if [ "$fails" -ne 0 ]; then
    echo "[FAIL] test_supervisor（$fails 项未过）"
    exit 1
fi
echo "[PASS] test_supervisor"
exit 0
