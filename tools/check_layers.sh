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

report() {
    echo "[FAIL] $1"
    head -5
    fail=1
}

echo "== 分层检查 =="

# 1. core 不碰 LVGL 与显示栈
out=$(grep -rnE '#include\s+["<](lvgl\.h|lvgl/|SDL2/|linux/)' app/core --include='*.c' --include='*.h')
[ -n "$out" ] && { echo "$out" | report "core 出现 LVGL/平台头文件"; } || echo "  [OK] core 无 LVGL/平台头文件"

# 2. core 里 cJSON 只允许出现在 remote/（远程通道的报文解析）
out=$(grep -rln 'cJSON\|cjson' app/core --include='*.c' --include='*.h' | grep -v 'app/core/remote/')
[ -n "$out" ] && { echo "$out" | report "core 非 remote 模块出现 cJSON"; } || echo "  [OK] core 仅 remote 模块使用 cJSON"

# 3. core 的 MQTT 封装不出现在 ui
out=$(grep -rn 'MQTTAsync\|paho' app/ui --include='*.c' --include='*.h')
[ -n "$out" ] && { echo "$out" | report "ui 出现 MQTT 底层实现"; } || echo "  [OK] ui 不接触 MQTT 底层"

# 4. hal 公共头文件不含平台实现头
for h in app/hal/hal_face.h app/hal/hal_time.h app/hal/hal_actuator.h app/hal/hal_camera.h app/hal/hal_storage.h; do
    out=$(grep -nE '#include\s+["<](linux/|SDL2/|sys/|termios|unistd\.h)' "$h")
    [ -n "$out" ] && { echo "$out" | report "$h 含平台头文件"; } || echo "  [OK] $h 干净"
done

# 5. core 不直接调用 LVGL API
out=$(grep -rnE '\blv_(obj|label|btn|timer|display)_' app/core --include='*.c')
[ -n "$out" ] && { echo "$out" | report "core 出现 LVGL API 调用"; } || echo "  [OK] core 无 LVGL API 调用"

# 6. main.c 只做引导（不出现业务调用）
out=$(grep -nE 'user_|log_append|backend_verify|totp_' app/main.c)
[ -n "$out" ] && { echo "$out" | report "main.c 出现业务逻辑"; } || echo "  [OK] main.c 只做引导"

if [ "$fail" -eq 0 ]; then
    echo "== 分层检查全部通过 =="
    exit 0
fi
exit 1
