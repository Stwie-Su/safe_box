#!/bin/bash
set -u
ROOT="${1:-.}"
cd "$ROOT" || exit 1

SCOPE=(app/ui app/core/remote app/core/auth/auth_fsm.c)

STORE_WRITE='(user_add|user_del|user_update|user_policy_set|user_face_set|user_del_cascade|log_append)[[:space:]]*\('

echo "== 主线程直连 store 写接口门禁 (QA-20 防回归) =="

mapfile -t HITS < <(grep -rnE "$STORE_WRITE" "${SCOPE[@]}" 2>/dev/null)

if [ "${#HITS[@]}" -eq 0 ]; then
    echo "  [OK] 未检出任何直接调用"
    exit 0
fi

enclosing_fn() {
    awk -v L="$1" '
        /^[A-Za-z_]/ && $0 !~ /^[A-Za-z_]+[[:space:]]*(if|for|while|switch|return|do|sizeof)[[:space:]]*\(/ {
            idx = match($0, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)
            if (idx > 0) {
                name = substr($0, idx, RLENGTH)
                sub(/[[:space:]]*\(.*$/, "", name)
                if (NR <= L) { fn = name; last = NR }
            }
        }
        END { if (last <= L) print fn }
    ' "$2"
}

# auth_fsm.c 全文件豁免 —— **已知遗留，勿误以为无需处理**：
#   FSM 的状态迁移与 UI 必须在主线程，其 user_update / log_append 因此仍在
#   主线程（含 submit_otp 路径：rpc.c:250 → auth_fsm.c:453 → unlock_backend.c
#   的 user_update）。经验证风险为「中」：需并发写盘且本地正处 OTP 会话才触发，
#   最坏是防暴力计数被覆盖而非数据丢失。
#   后续应把 FSM 的 user_update 也经 worker 收口，届时移除本豁免。
is_whitelisted() {
    case "$1" in
        app/core/auth/auth_fsm.c) return 0 ;;   # 豁免理由见函数上方注释
        *) return 1 ;;
    esac
}

violations=0
for hit in "${HITS[@]}"; do
    f="${hit%%:*}"
    rest="${hit#*:}"
    ln="${rest%%:*}"
    text="${hit#*:*:}"
    fn="$(enclosing_fn "$ln" "$f")"
    case "$fn" in
        *_worker|rpc_job_fn) continue ;;
    esac
    case "$text" in
        *user_del_cascade*) continue ;;
        # 行内显式标记 GATE-EXEMPT：作者已确认该处属有意设计，
        # 比硬编码行号稳定（行号一变就失效，标记不会）。
        *GATE-EXEMPT*) continue ;;
    esac
    if is_whitelisted "$f" "$ln"; then
        echo "  [白名单] $f:$ln ($fn) :: $text"
        continue
    fi
    echo "  [违规] $f:$ln ($fn) :: $text"
    violations=$((violations + 1))
done

if [ "$violations" -gt 0 ]; then
    echo "== 门禁失败：$violations 处主线程直接调用 store 写接口（QA-20 类） =="
    exit 1
fi
echo "== 门禁通过：未见新增的主线程直连 store 写接口 =="
exit 0
