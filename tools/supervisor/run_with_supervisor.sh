#!/bin/sh
# ============================================================
#  板子启动入口：用 supervisor 包裹应用（方案 B 落地）
#
#  用法一（推荐，改动最小）：把 /etc/init.d/S05lvgl 里「直接启动应用」的那一行
#      改为调用本脚本：
#          /usr/share/lvgl/run_with_supervisor.sh &
#  用法二（inittab）：
#          ::respawn:/usr/share/lvgl/safe_supervisor --app /usr/share/lvgl/lvglsim
#
#  ★ 互斥关系（不要叠加）：
#      supervisor 自己已经负责「崩溃重启 + 限速 + 安全态」。
#      如果再用 inittab 的 respawn 去复活 supervisor，那么 supervisor 进入安全态
#      后退出（rc=2）会被 init 立刻重新拉起 → 应用又被启动 → **安全态被绕过**。
#      二选一：要么 S05 一次性启动本脚本（由 supervisor 管重启），
#              要么 inittab respawn 直接跑应用（那就不需要 supervisor）。
#
#  ★ 当前状态：板子未接入，本脚本未经板上验证。先在板上手工执行一次确认行为，
#      再替换 S05 的启动方式（不要直接覆盖现网可用的 S05lvgl）。
#
#  可用环境变量覆盖：
#      SAFE_APP            应用路径（默认 /usr/share/lvgl/lvglsim）
#      SAFE_SUPERVISOR     supervisor 路径（默认同目录 safe_supervisor）
#      SAFE_MAX_RESTARTS   滑动窗口内允许的重启次数（默认 5）
#      SAFE_WINDOW         滑动窗口秒数（默认 60）
#      SAFE_GRACE          优雅停机宽限秒数（默认 5）
#      SAFE_SAFE_CMD       安全态动作（默认 /etc/safe/safe_state.sh）
#      SAFE_SUP_LOG        日志路径（默认 /var/log/safe_supervisor.log）
# ============================================================
set -u

APP="${SAFE_APP:-/usr/share/lvgl/lvglsim}"
SUP="${SAFE_SUPERVISOR:-/usr/share/lvgl/safe_supervisor}"
LOG="${SAFE_SUP_LOG:-/var/log/safe_supervisor.log}"

if [ ! -x "$SUP" ]; then
    echo "[run_with_supervisor] supervisor 不存在或不可执行: $SUP —— 回退直接启动应用" >&2
    exec "$APP"
fi

exec "$SUP" --app "$APP" \
    --max-restarts "${SAFE_MAX_RESTARTS:-5}" \
    --window "${SAFE_WINDOW:-60}" \
    --grace "${SAFE_GRACE:-5}" \
    --safe-cmd "${SAFE_SAFE_CMD:-/etc/safe/safe_state.sh}" \
    --log "$LOG"
