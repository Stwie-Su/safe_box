#!/bin/bash
# FM225 真模组后端启动器（单实例守卫 + 串口走稳定 by-id + 相机交给自动扫描）
cd "$(dirname "$0")"
if pgrep -x lvglsim >/dev/null 2>&1; then
  echo "[run_fm225] lvglsim 已在运行，本启动器退出"
  exit 0
fi
export SAFE_FACE_BACKEND=fm225
export SAFE_FM225_DEV=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
# 相机不写死路径：交给 v4l2 自动扫描 /dev/video0..15，自适应重插后的节点号变化
exec stdbuf -oL -eL ./build_pc/bin/lvglsim
