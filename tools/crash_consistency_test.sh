#!/bin/bash
# 崩溃一致性长跑驱动（技术路线规约 §10 风险项 R1 的验收项）。
#
# test_store_crash 二进制已闭环实现「写盘 → 随机时刻 kill -9 → 校验可解析且要么
# 全旧要么全新」，本脚本只负责定位二进制并放大轮次，方便人工长跑与留档。
#
# 用法：tools/crash_consistency_test.sh [轮次] [构建目录]
#   轮次   默认 50（ctest 里默认 20，这里默认更大以便压出问题）
#   构建目录 默认 build_pc
set -u

ROUNDS="${1:-50}"
BUILD="${2:-build_pc}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/$BUILD/bin/test_store_crash"

if [ ! -x "$BIN" ]; then
    echo "[FAIL] 找不到可执行文件：$BIN"
    echo "       先构建：cmake --build $BUILD -j4"
    exit 1
fi

echo "== 崩溃一致性自测：$ROUNDS 轮（二进制 $BIN）=="
"$BIN" "$ROUNDS"
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "[FAIL] 崩溃一致性自测未通过（退出码 $rc）"
    exit "$rc"
fi

echo "[PASS] 崩溃一致性自测通过：任意时刻 kill -9 后 users.json 均可解析且非旧即新"
exit 0
