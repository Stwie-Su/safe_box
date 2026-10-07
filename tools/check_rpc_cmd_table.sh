#!/bin/bash
# RPC 指令相关名单的一致性门禁。
#
# 背景：dispatch() 用一串 strcmp(c, "xxx") 表达「支持哪些指令」，而入口的 OTP
# 统一门控还需要另外两张表（「已实现指令表」与「自带补码流程」豁免名单）。于是
# 「支持哪些指令 / 谁自己管补码」这一事实有了多处表达 —— 靠人同步必漏
# （D20「复制代码必漏初始化」、D22b、D25 都是同源教训），必须机械检查。
#
# 三条检查：
#   1. dispatch 承接的指令 == rpc_guard 的已实现指令表
#      不一致 → 一侧漏改（新增指令忘了登记 = 功能静默不可用）。
#   2. 豁免名单 ⊆ 已实现指令表
#      豁免了一个不存在的指令名（拼错 / 指令已删）→ 该豁免永不生效。
#   3. 豁免名单必须含 remote_unlock / sync_time
#      ★ 这条是**检查 2 补不上**的漏洞：整条漏登记之后，剩余的项仍然是指令表的
#        子集，检查 2 照样绿，而这两条指令会被入口门控拦下并**静默失效** ——
#        remote_unlock 丢掉 ADR-4 的本地弹动态码页，sync_time 丢掉 ≤3600s 的
#        常规 NTP 校时。失败方向不对称，是三张表里唯一「漏登记即不安全」的那张。
#
# ★ 抽取实现的两个硬约束（改源码写法时务必同步）：
#   a) 指令名字符集 = [A-Za-z0-9_]。写成别的字符（含中文/连字符）会**两侧都抽
#      不到**，两边集合仍然相等 → 门禁报绿却没在检查（假阴性，比误报危险得多）。
#   b) 已实现指令表必须以 `};` 收尾、豁免函数以 `}` 收尾 —— 抽取按这两个定界符
#      取范围，**与表有多少条、一行放几个无关**（早期版本用 grep -A20 固定窗口，
#      表超过 20 条就会漏抽并误报 FAIL，已修）。
#
# 用法：check_rpc_cmd_table.sh <工程根目录>
set -u
export LC_ALL=C
ROOT="${1:-.}"
cd "$ROOT" || exit 1

GUARD=app/core/remote/rpc_guard.c
DISPATCH=app/core/remote/rpc.c
NAME_RE='[A-Za-z0-9_]+'

# 抽取 1：dispatch 实际承接的指令（所有 strcmp(c, "xxx")）
in_dispatch=$(grep -oE "strcmp\(c, \"$NAME_RE\"\)" "$DISPATCH" \
              | sed -E "s/.*\"($NAME_RE)\".*/\1/" | sort -u)

# 抽取 2：rpc_guard 的「已实现指令表」s_known_cmds（起始行 -> `};`，与长度无关）
in_table=$(sed -n '/s_known_cmds\[\] = {/,/^};/p' "$GUARD" \
           | grep -oE "\"$NAME_RE\"" | tr -d '"' | sort -u)

# 抽取 3：「自带补码流程」豁免名单（函数起始行 -> `}`）
in_exempt=$(sed -n '/^bool rpc_cmd_otp_self_escalating/,/^}/p' "$GUARD" \
            | grep -oE "\"$NAME_RE\"" | tr -d '"' | sort -u)

echo "== RPC 指令清单一致性门禁 =="

if [ -z "$in_dispatch" ] || [ -z "$in_table" ]; then
    echo "  [违规] 有一侧抽不到任何指令名"
    echo "         dispatch=$(printf '%s' "$in_dispatch" | wc -l) 条 / 指令表=$(printf '%s' "$in_table" | wc -l) 条"
    echo "         多半是抽取正则与源码写法脱节，请同步修本脚本。"
    echo "== 门禁失败：指令清单抽取异常 =="
    exit 1
fi

fail=0

# ---- 检查 1：dispatch 与已实现指令表一一对应 ----
if [ "$in_dispatch" != "$in_table" ]; then
    echo "  [违规] dispatch 承接的指令与 rpc_guard 登记的指令表不一致："
    diff <(printf '%s\n' "$in_dispatch") <(printf '%s\n' "$in_table") \
        | sed 's/^</    仅 dispatch 有：/; s/^>/    仅指令表有：/' | grep -E '^(    仅|---)' || true
    echo "  → 新增/删除指令必须同时改：rpc.c::dispatch() 的 strcmp 分支、"
    echo "    rpc_guard.c 的 s_known_cmds 表，并按需改 needs_otp / otp_self_escalating。"
    fail=1
else
    n=$(printf '%s\n' "$in_dispatch" | sed '/^$/d' | wc -l | tr -d ' ')
    echo "  [OK] dispatch 与 rpc_guard 指令表一致（$n 条）"
fi

# ---- 检查 2：豁免名单必须是已实现指令表的子集 ----
not_known=""
for e in $in_exempt; do
    if ! printf '%s\n' "$in_table" | grep -qx "$e"; then
        not_known="$not_known $e"
    fi
done
if [ -n "$not_known" ]; then
    echo "  [违规] 豁免名单里的这些名字不在已实现指令表中（拼错 / 指令已删除）：$not_known"
    echo "         → 该豁免永远不生效，而真正需要豁免的那条指令可能正因此没被豁免。"
    fail=1
else
    echo "  [OK] 自带补码流程的豁免名单是已实现指令表的子集"
fi

# ---- 检查 3：豁免名单必须包含这两条（子集检查挡不住「整条漏登记」） ----
MUST_ESCALATE="remote_unlock sync_time"
for m in $MUST_ESCALATE; do
    if ! printf '%s\n' "$in_exempt" | grep -qx "$m"; then
        echo "  [违规] 豁免名单缺少 '$m' —— 它的分支自带补码流程，被入口门控拦下会静默失效："
        echo "         remote_unlock → 丢掉 ADR-4 的本地弹动态码页；"
        echo "         sync_time     → 丢掉 |Δ| <= 3600s 的常规 NTP 校时。"
        fail=1
    fi
done
if [ "$fail" -eq 0 ]; then
    echo "  [OK] 豁免名单含 remote_unlock / sync_time"
fi

if [ "$fail" -ne 0 ]; then
    echo "== 门禁失败：RPC 指令相关名单不一致 =="
    exit 1
fi
echo "== 门禁通过：三张指令相关名单一致 =="
exit 0
