#!/bin/bash
# RPC 指令清单一致性门禁。
#
# 背景：dispatch() 用一串 strcmp(c, "xxx") 表达「支持哪些指令」，而 OTP 统一门控
# 还需要另外两张表（rpc_guard 的「已实现指令表」与「自带补码流程」名单）。于是
# 「支持哪些指令」这一事实有了两处表达 —— 靠人同步必漏（D20「复制代码必漏初始化」、
# D22b、D25 都是同源教训）。本脚本把两处抽出来机械比对。
#
# 为什么要比：
#   · 新增指令只改 dispatch、没进指令表  → 该指令被当未知指令拒（失败方向安全，
#                                          但功能静默不可用，属缺陷）；
#   · 漏登记「自带补码流程」              → remote_unlock 的 ADR-4 本地弹码页与
#                                          sync_time 的常规校时被入口门控吃掉，
#                                          **静默失效**（失败方向不安全）。
#
# 用法：check_rpc_cmd_table.sh <工程根目录>
set -u
ROOT="${1:-.}"
cd "$ROOT" || exit 1

GUARD=app/core/remote/rpc_guard.c
DISPATCH=app/core/remote/rpc.c

# (1) dispatch 里实际承接的指令：所有 strcmp(c, "xxx")
in_dispatch=$(grep -oE 'strcmp\(c, "[a-z_]+"\)' "$DISPATCH" \
              | sed -E 's/.*"([a-z_]+)".*/\1/' | sort -u)

# (2) rpc_guard 里登记的「已实现指令表」（s_known_cmds）。
#     按固定串定位后取其后 20 行里的引号字符串；指令表是一行一个，故不会漏抽。
in_table=$(grep -A20 -F 's_known_cmds[] = {' "$GUARD" \
           | grep -oE '"[a-z_]+"' | tr -d '"' | sort -u)

echo "== RPC 指令清单一致性门禁 =="

if [ -z "$in_dispatch" ] || [ -z "$in_table" ]; then
    echo "  [违规] 有一侧抽不到任何指令名（dispatch=$(printf '%s' "$in_dispatch" | wc -l) 条 / 指令表=$(printf '%s' "$in_table" | wc -l) 条）"
    echo "         多半是抽取正则与源码写法脱节，请同步修本脚本。"
    echo "== 门禁失败：指令清单抽取异常 =="
    exit 1
fi

if [ "$in_dispatch" != "$in_table" ]; then
    echo "  [违规] dispatch 承接的指令与 rpc_guard 登记的指令表不一致："
    diff <(printf '%s\n' "$in_dispatch") <(printf '%s\n' "$in_table") \
        | sed 's/^</    仅 dispatch 有：/; s/^>/    仅指令表有：/' | grep -E '^(    仅|---)' || true
    echo "  → 新增/删除指令必须同时改：rpc.c::dispatch() 的 strcmp 分支、"
    echo "    rpc_guard.c 的 s_known_cmds 表，并按需改 needs_otp / otp_self_escalating。"
    echo "== 门禁失败：两份指令清单不一致 =="
    exit 1
fi

n=$(printf '%s\n' "$in_dispatch" | sed '/^$/d' | wc -l | tr -d ' ')
echo "  [OK] dispatch 与 rpc_guard 指令表一致（$n 条）"
exit 0
