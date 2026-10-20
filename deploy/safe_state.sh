#!/bin/sh
# ============================================================
#  安全态动作钩子（supervisor --safe-cmd 的默认目标）
#
#  什么时候被调用：
#    应用在滑动窗口内反复崩溃超过 --max-restarts，supervisor 判定
#    「设备异常」，停止重启并调用本脚本，然后接管硬件看门狗持续喂狗。
#    设备保持运行、保持锁闭，等人来修；不会无限整机复位。
#
#  这个脚本必须满足两条：
#    1) 失效导向安全侧：真锁接入后在这里把继电器拉低（保持锁闭），
#       而不是打开锁；任何失败都不能让锁处于「开」的状态。
#    2) 可重入：被重复调用不产生副作用（写日志是幂等的）。
#
#  当前硬件状态：电磁锁 GPIO 后端尚未接入（CLAUDE.md §10），
#  所以这里只记录；真锁到货后取消下面 GPIO 示例的注释并改成实际引脚。
# ============================================================

# 留痕：syslog（串口可见）+ 独立文件（掉电后仍可查）
logger -t safe "SAFE-STATE: 反复崩溃，保持锁闭等待人工介入" 2>/dev/null
echo "$(date '+%Y-%m-%d %H:%M:%S') SAFE-STATE 保持锁闭" >> /var/log/safe_state.log 2>/dev/null

# 保持锁闭：把执行器输出拉到「不开锁」电平（低 = 锁闭，见 hal_actuator.h）。
# 引脚与 sysfs 根目录跟应用共用同一组环境变量，避免两处各写一个编号：
#   应用侧 hal_actuator（backend_gpio.c）读的也是 SAFE_GPIO_ROOT / SAFE_ACTUATOR_GPIO。
# GPIO4_19 全局编号 = (4-1)*32 + 19 = 115（i.MX6ULL 常见算法）——以板上实际
# 导出结果为准，换接线只改环境变量，不改脚本。
GPIO_ROOT=${SAFE_GPIO_ROOT:-/sys/class/gpio}
GPIO_PIN=${SAFE_ACTUATOR_GPIO:-115}
GPIO_VAL="$GPIO_ROOT/gpio$GPIO_PIN/value"
if [ -w "$GPIO_VAL" ]; then
    if echo 0 > "$GPIO_VAL" 2>/dev/null; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') SAFE-STATE 已拉低 GPIO$GPIO_PIN（保持锁闭）" >> /var/log/safe_state.log 2>/dev/null
        logger -t safe "SAFE-STATE: GPIO$GPIO_PIN 已拉低" 2>/dev/null
    else
        echo "$(date '+%Y-%m-%d %H:%M:%S') SAFE-STATE 拉低 GPIO$GPIO_PIN 失败" >> /var/log/safe_state.log 2>/dev/null
    fi
else
    # 引脚还没导出（真锁未接入 / 被其它驱动占用）：只记录，不影响流程
    echo "$(date '+%Y-%m-%d %H:%M:%S') SAFE-STATE 未找到 $GPIO_VAL（真锁未接入？）" >> /var/log/safe_state.log 2>/dev/null
fi

# 安全态下无人工介入不应自动恢复：不重启应用、不清理日志。
exit 0
