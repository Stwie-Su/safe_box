#!/bin/bash
set -u
ROOT="${1:-.}"
cd "$ROOT" || exit 1

# ★ 2026-10-07：auth_fsm.c 的整文件豁免已摘除（认证路径零同步落盘落地），
#   并把 unlock_backend.c 纳入扫描 —— 此前它是个盲区：backend_verify_totp 内部
#   就有 user_find_by_name + user_update（读盘 + 整表写盘），而它曾被主线程
#   经 auth_fsm_submit_otp 同步调用，门禁完全看不见。
SCOPE=(app/ui app/core/remote app/core/auth/auth_fsm.c app/core/auth/unlock_backend.c)

# ★ 不变式已升级（QA-20 收尾）：users.json / safe.log 的**读与写**全部由 worker
#   线程独占持有，主线程不得调用任何 store 读写接口（要数据走 worker_post 三段式，
#   或读 store_cache 快照）。因此门禁从「只查写接口」扩到「读写都查」。
STORE_ACCESS='(user_add|user_del|user_update|user_policy_set|user_face_set|user_del_cascade'
STORE_ACCESS+='|user_load_all|user_find_by_id|user_find_by_name|user_find_by_face|user_next_id'
STORE_ACCESS+='|user_verify_pin|log_append|log_query)[[:space:]]*\('

echo "== 主线程直连 store 读写接口门禁 (QA-20 防回归) =="

mapfile -t HITS < <(grep -rnE "$STORE_ACCESS" "${SCOPE[@]}" 2>/dev/null)

# ---- GATE-EXEMPT 行内豁免：给一个**显式上限** ----
# 行内 GATE-EXEMPT 的本意是「作者已确认该处属有意设计」，但它同时是一个
# **可无限滥用的口子**：在任何 store 调用后面加一句注释就能绕过本门禁，
# 且不需要经过任何人审查 —— 门禁对新代码实际上形同虚设。
# 因此这里给它计数上限：当前全工程仅 rpc.c 一处（job 分配失败时的审计日志兜底）。
# 新增豁免必须**同时上调 GATE_EXEMPT_MAX** —— 把「悄悄绕过」变成一次显式的、
# 在 diff 里一眼可见的动作（改门禁脚本本身是要过 review 的）。
GATE_EXEMPT_MAX=1
mapfile -t EXEMPT_HITS < <(grep -rn 'GATE-EXEMPT' app 2>/dev/null)
if [ "${#EXEMPT_HITS[@]}" -gt "$GATE_EXEMPT_MAX" ]; then
    echo "  [违规] GATE-EXEMPT 行内豁免 ${#EXEMPT_HITS[@]} 处，超过上限 ${GATE_EXEMPT_MAX} 处："
    printf '    %s\n' "${EXEMPT_HITS[@]}"
    echo "== 门禁失败：GATE-EXEMPT 超额（新增豁免必须同步上调本脚本的 GATE_EXEMPT_MAX）=="
    exit 1
fi
echo "  [OK] GATE-EXEMPT 行内豁免 ${#EXEMPT_HITS[@]} 处（上限 ${GATE_EXEMPT_MAX} 处）"

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
# ★ D24（2026-10-02）：page_users.c 拆分为 rows/dlg 两文件，下列 5 个
#   点击路径读 store 的函数整体搬入 page_users_dlg.c，白名单文件名同步跟进
#   （函数与调用语义零改动，纯搬移）。
legacy_case() {
    case "$1::$2" in
        # ★ 2026-10-07 摘除：原先 auth_fsm.c 是**整文件豁免**（::*），是这张名单里
        #   唯一的「按文件」口子，其余都是按函数名精确登记。现在认证路径的读走
        #   store_cache 快照、写走 astore_auth_side / astore_append_log，
        #   文件内已无任何直接 store 调用 —— 整文件豁免失去存在理由。
        #   代价要写清：FSM 状态迁移仍在主线程，落盘结果晚一帧才可见；
        #   安全判定（失败计数 / 锁定 / 防重放）全部依赖 worker 单线程 FIFO 的顺序保证。
        # unlock_backend.c 的 backend_* 是「存储层判定封装」：它们内部的 store 调用
        # 是设计的一部分（PIN / TOTP 校验必须读真实文件、回写防重放计数器）。
        # ★ 调用方纪律（本条豁免成立的前提，改动时务必复核）：这些函数**只允许在
        #   worker 线程调用**——PIN 走 astore/pin_worker、TOTP 走 otp_worker、
        #   管理员二次确认走 astore_verify_admin。新增任何**非 backend_ 前缀**
        #   的函数若在文件内直连 store，门禁会拦下。
        app/core/auth/unlock_backend.c::backend_*)      return 0 ;;
        # backend 层的内部辅助：只被 backend_verify_pin_ex 调用，与调用方同线程（worker）。
        # 单独登记而不是靠 backend_ 前缀通配——前缀豁免只给**对外接口**，
        # 内部辅助逐个登记，新增一个就会被门禁拦下要求显式说明。
        app/core/auth/unlock_backend.c::temp_revoke)    return 0 ;;
        app/ui/pages/page_users_dlg.c::user_is_admin)       return 0 ;;  # 全表读判「是否有管理员」
        app/ui/pages/page_users_dlg.c::face_btn_cb)         return 0 ;;  # 人脸按钮态需当拍拿到 face_id
        app/ui/pages/page_users_dlg.c::face_start_cb)       return 0 ;;
        app/ui/pages/page_users_dlg.c::page_users_retry_enroll) return 0 ;;
        app/ui/pages/page_users_dlg.c::do_del_cb)           return 0 ;;  # 删除前取用户名写审计
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
