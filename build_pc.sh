#!/bin/bash
# PC/SDL 构建脚本（cmake --build build_pc 的便捷包装，与 run_fm225.sh 同款体验）。
# 用法：./build_pc.sh              # 增量构建
#       ./build_pc.sh --clean-first  # 传任意 cmake 参数也行
cd "$(dirname "$0")"
exec cmake --build build_pc -j4 "$@"
