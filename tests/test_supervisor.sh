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

if [ "$fails" -ne 0 ]; then
    echo "[FAIL] test_supervisor（$fails 项未过）"
    exit 1
fi
echo "[PASS] test_supervisor"
exit 0
