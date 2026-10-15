#!/bin/bash
# 分层检查（docs/02-架构设计.md §1）：
#   app/core   禁止 LVGL / 平台头文件 / cJSON（remote 除外）
#   app/hal    公共头文件禁止平台实现
#   app/ui     禁止平台底层协议头
# 用法：check_layers.sh <工程根目录>
set -u
ROOT="${1:-.}"
cd "$ROOT" || exit 1
fail=0

# ★ 曾踩过的坑：写成 `echo "$out" | report "..."` 会让 report 跑在**子 shell** 里，
#   里面设的 fail=1 出不来 —— 于是检查形同虚设（FAIL 打印了，退出码却是 0）。
#   改为把内容作为参数传进去。
report() {
    echo "[FAIL] $1"
    if [ -n "${2:-}" ]; then echo "$2" | head -5; fi
    fail=1
}

echo "== 分层检查 =="

# 1. core 不碰 LVGL 与显示栈
out=$(grep -rnE '#include\s+["<](lvgl\.h|lvgl/|SDL2/|linux/)' app/core --include='*.c' --include='*.h')
[ -n "$out" ] && { report "core 出现 LVGL/平台头文件" "$out"; } || echo "  [OK] core 无 LVGL/平台头文件"

# 2. core 里 cJSON 只允许出现在 remote/（远程通道的报文解析）
out=$(grep -rln 'cJSON\|cjson' app/core --include='*.c' --include='*.h' | grep -v 'app/core/remote/')
[ -n "$out" ] && { report "core 非 remote 模块出现 cJSON" "$out"; } || echo "  [OK] core 仅 remote 模块使用 cJSON"

# 3. 不依赖第三方 MQTT 库（R9 起自研：板子上没有可用的 ARM 版第三方库，
#    依赖它会让交叉构建静默退化成 stub → 板上远程通道永久断线。
#    这条把「不再引入第三方 MQTT 依赖」变成**机械检查**，防止回退。）
out=$(grep -rniE 'MQTTAsync|MQTTClient|paho-mqtt|libpaho' app --include='*.c' --include='*.h')
[ -n "$out" ] && { report "app 出现第三方 MQTT 库依赖" "$out"; } || echo "  [OK] app 不依赖第三方 MQTT 库"

# 4. hal 公共头文件不含平台实现头
for h in app/hal/hal_face.h app/hal/hal_time.h app/hal/hal_actuator.h app/hal/hal_camera.h app/hal/hal_storage.h app/hal/hal_net.h app/hal/hal_watchdog.h; do
    out=$(grep -nE '#include\s+["<](linux/|SDL2/|sys/|termios|unistd\.h)' "$h")
    [ -n "$out" ] && { report "$h 含平台头文件" "$out"; } || echo "  [OK] $h 干净"
done

# 5. core 不直接调用 LVGL API
out=$(grep -rnE '\blv_(obj|label|btn|timer|display)_' app/core --include='*.c')
[ -n "$out" ] && { report "core 出现 LVGL API 调用" "$out"; } || echo "  [OK] core 无 LVGL API 调用"

# 6. main.c 只做引导（不出现业务调用）
out=$(grep -nE 'user_|log_append|auth_verify|totp_' app/main.c)
[ -n "$out" ] && { report "main.c 出现业务逻辑" "$out"; } || echo "  [OK] main.c 只做引导"

# 7. 依赖方向扫描（★ 当前**只告警不失败**，原因见下）
#    背景：上面 1~6 全是「内容扫描」（某个头 / 某个 API 有没有出现），
#    **完全没有检查依赖方向**。于是 ui->hal、hal->core、core->platform 这类反向
#    依赖整片落在门禁之外 —— 门禁常绿，给出的却是错误的安全感。这里补上方向扫描。
#
#    ⚠️ 为什么先只 WARNING：
#      · 存量违规不少，一次性 FAIL 会直接阻塞 CI，而现实反应通常是「把门禁关掉」，
#        那比没有门禁更糟 —— 门禁必须保持在「绿了就能合」的可信状态。
#      · ui->hal **对外接口头文件**（hal/hal_*.h）有 25 处历史现状，且规约 §1.1 与 §5.19
#        对「ui 能否直接用 hal 对外接口头文件」表述本身矛盾，属**待定项**，不适合现在判死。
#    故：所有方向先按 WARNING 输出（前 5 条 + 总数）。其中「ui 引用 hal **内部子目录
#    头**」是明确的违规档（对外接口头文件之约已存在，内部头不该被 UI 直接看见），
#    待逐项收敛后把 DIR_SCAN_ENFORCE 置 1 即转为 FAIL。
#    TODO(架构债)：ui->hal 对外接口头文件 / hal->core / core->platform 逐项收敛后转 FAIL。
DIR_SCAN_ENFORCE=0      # 1 = FAIL-worthy 方向直接判失败（存量收敛后再开）

warn_dir() {
    # $1 = 方向名   $2 = 档位（WARN / FAIL-worthy）   $3 = 命中文本
    local n
    n=$(printf '%s\n' "$3" | sed '/^$/d' | wc -l | tr -d ' ')
    if [ "$n" -eq 0 ]; then
        echo "  [OK] $1 无违规"
        return 0
    fi
    echo "  [方向违规/$2] $1 —— 共 $n 处："
    printf '%s\n' "$3" | sed '/^$/d' | head -5 | sed 's/^/      /'
    if [ "$n" -gt 5 ]; then echo "      ……（其余 $((n - 5)) 处略）"; fi
    if [ "$2" = "FAIL-worthy" ] && [ "$DIR_SCAN_ENFORCE" -eq 1 ]; then
        fail=1
    fi
    return 0
}

echo "  -- 依赖方向扫描（当前为 WARNING，不计入退出码） --"

# (a) hal -> core（例外：core/err.h 是全层共用的错误码定义，不算业务依赖）
d=$(grep -rnE '#include[[:space:]]*"core/' app/hal --include='*.c' --include='*.h' 2>/dev/null | grep -vE 'core/err\.h')
warn_dir "hal -> core（例外 core/err.h）" "WARN" "$d"

# (b) core -> platform（例外：platform/thread_util.h 要在 <pthread.h> 之前包含以
#     定义 _GNU_SOURCE，属编译期必需而非业务依赖）
d=$(grep -rnE '#include[[:space:]]*"platform/' app/core --include='*.c' --include='*.h' 2>/dev/null | grep -vE 'platform/thread_util\.h')
warn_dir "core -> platform（例外 platform/thread_util.h）" "WARN" "$d"

# (c) platform -> ui|core|hal（例外：platform/debug_hooks.c 是调试钩子，历史遗留，
#     登记为例外）
d=$(grep -rnE '#include[[:space:]]*"(ui|core|hal)/' app/platform --include='*.c' --include='*.h' 2>/dev/null | grep -vE '^app/platform/debug_hooks\.c:')
warn_dir "platform -> ui|core|hal（例外 platform/debug_hooks.c）" "WARN" "$d"

# (d) ui -> hal **对外接口头文件**（hal/hal_*.h）：25 处历史现状 + 规约表述矛盾（待定项）
#     → 只 WARNING，**不得 FAIL**
d=$(grep -rnE '#include[[:space:]]*"hal/hal_[^/]*\.h"' app/ui --include='*.c' --include='*.h' 2>/dev/null)
warn_dir "ui -> hal 对外接口头文件（hal/hal_*.h，规约待定项）" "WARN" "$d"

# (e) ui -> hal **内部子目录头**（hal/face/face_thread.h 这类）：对外接口头文件之外，明确违规
# ★ 正则必须写成 [^"/]+ 而不是 [^/]+：后者会一路吃到行尾块注释里的 '/'
#   （`#include "hal/hal_time.h"   /* 说明 */`），把**对外接口头文件**误算成内部子目录头。
d=$(grep -rnE '#include[[:space:]]*"hal/[^"/]+/' app/ui --include='*.c' --include='*.h' 2>/dev/null)
warn_dir "ui -> hal 内部子目录头" "FAIL-worthy" "$d"

if [ "$fail" -eq 0 ]; then
    echo "== 分层检查全部通过 =="
    exit 0
fi
exit 1
