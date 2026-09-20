#!/bin/bash
set -u
ROOT="${1:-.}"
cd "$ROOT" || exit 1

SCOPE=(app/ui app/core/remote app/core/auth/auth_fsm.c)

# ★ 不变式已升级（QA-20 收尾）：users.json / safe.log 的**读与写**全部由 worker
#   线程独占持有，主线程不得调用任何 store 读写接口（要数据走 worker_post 三段式，
#   或读 store_cache 快照）。因此门禁从「只查写接口」扩到「读写都查」。
STORE_ACCESS='(user_add|user_del|user_update|user_policy_set|user_face_set|user_del_cascade'
STORE_ACCESS+='|user_load_all|user_find_by_id|user_find_by_name|user_find_by_face|user_next_id'
STORE_ACCESS+='|user_verify_pin|log_append|log_query)[[:space:]]*\('

echo "== 主线程直连 store 读写接口门禁 (QA-20 防回归) =="

mapfile -t HITS < <(grep -rnE "$STORE_ACCESS" "${SCOPE[@]}" 2>/dev/null)

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

# 遗留白名单（**已知遗留，勿误以为无需处理**）。
# 与 auth_fsm.c 同一取舍：这些是**改动前就存在**的主线程直读路径，UI 同步渲染
# 需要当场拿到数据（user_find_by_id 决定按钮态、log_query 数今日事件条数……）。
# 收口它们要么给 store_cache 补「按 face_id 查 / 全量遍历 / 日志计数」接口，
# 要么把 UI 改成异步回调式 —— 等于重写用户管理页 / 监控页 / 日志页，
# 与本次 R9 主线（自研 MQTT + 重活下沉）不成比例，故**本次明确不做**。
# ★ 白名单按「文件::函数」登记而不是行号：行号一改就失效，函数名不会。
#   新增文件 / 新增函数一律不在名单内 → 门禁对新代码仍然有效。
legacy_case() {
    case "$1::$2" in
        # FSM 的状态迁移与 UI 必须在主线程，其 user_update / log_append 因此仍在
        # 主线程（含 submit_otp 路径）。与 FSM 状态迁移强耦合，A6 裁定本次不动。
        app/core/auth/auth_fsm.c::*)                    return 0 ;;
        app/ui/pages/page_users.c::user_is_admin)       return 0 ;;  # 全表读判「是否有管理员」
        app/ui/pages/page_users.c::face_btn_cb)         return 0 ;;  # 人脸按钮态需当拍拿到 face_id
        app/ui/pages/page_users.c::face_start_cb)       return 0 ;;
        app/ui/pages/page_users.c::page_users_retry_enroll) return 0 ;;
        app/ui/pages/page_users.c::do_del_cb)           return 0 ;;  # 删除前取用户名写审计
        app/ui/pages/page_settings.c::refresh_log_count)  return 0 ;; # 日志条数（创建/手动刷新时一次）
        app/ui/pages/page_monitor.c::count_today_events)  return 0 ;; # 今日事件计数（全量 log_query 后过滤）
        app/ui/ui_feedback.c::detect_text)                return 0 ;; # 按 face_id 反查用户名做提示
        *) return 1 ;;
    esac
}

violations=0
legacy=0
for hit in "${HITS[@]}"; do
    f="${hit%%:*}"
    rest="${hit#*:}"
    ln="${rest%%:*}"
    text="${hit#*:*:}"
    # 注释里提到函数名不算违规（文档会写「这里不能调 xxx()」，正则命中不了语义）。
    # 只在「去掉行首空白后以 * 或 / 或 # 开头」时跳过 —— 即块注释 / 行注释 / 预处理。
    stripped="${text#"${text%%[![:space:]]*}"}"
    case "$stripped" in
        '*'*|'/'*|'#'*) continue ;;
    esac
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
    if legacy_case "$f" "$fn"; then
        echo "  [遗留白名单] $f:$ln ($fn) :: $text"
        legacy=$((legacy + 1))
        continue
    fi
    echo "  [违规] $f:$ln ($fn) :: $text"
    violations=$((violations + 1))
done

if [ "$violations" -gt 0 ]; then
    echo "== 门禁失败：$violations 处主线程直接调用 store 读写接口（QA-20 类） =="
    exit 1
fi
echo "== 门禁通过：无新增的主线程直连 store 读写接口（遗留白名单命中 $legacy 处，见脚本内注释） =="
exit 0
