cmake_minimum_required(VERSION 3.16)

# ============================================================
#  智能保险柜 · 顶层构建（LVGL v9）
# ------------------------------------------------------------
#  平台差异不写在本文件，由 defconfig 决定：
#    configs/pc.defconfig       Ubuntu 桌面 SDL2（开发验证，默认）
#    configs/board.defconfig    i.MX6ULL 板子 FBDEV + EVDEV（交叉编译）
#  可选特性在 cmake/SafeFeatures.cmake 里探测，缺依赖自动降级，
#  保证核心链路在任何环境下都能编译。
# ============================================================
project(safe_lvgl
    VERSION 1.0.0
    DESCRIPTION "多用户共享保险柜 · LVGL v9"
    LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

find_package(Threads REQUIRED)   # LV_OS_PTHREAD 与后端需要
find_package(Python3 REQUIRED COMPONENTS Interpreter)   # LVGL Kconfig 生成需要

include(SafeOptions)
include(SafeFeatures)
include(SafeVersion)

# ---------------- LVGL 子项目：Kconfig + defconfig ----------------
set(LV_BUILD_USE_KCONFIG ON CACHE BOOL "使用 Kconfig" FORCE)
set(LV_BUILD_KCONFIG_ROOT "${CMAKE_CURRENT_LIST_DIR}/Kconfig" CACHE PATH "Kconfig 根" FORCE)

set(LV_PORT_DEFCONFIG "configs/pc.defconfig" CACHE STRING
    "选择配置：configs/pc.defconfig(PC/SDL) 或 configs/board.defconfig(板子/FBDEV+EVDEV)")
set(LV_BUILD_DEFCONFIG_PATH "${CMAKE_CURRENT_LIST_DIR}/${LV_PORT_DEFCONFIG}" CACHE PATH "defconfig 路径" FORCE)
unset(LV_BUILD_DOTCONFIG_PATH CACHE)
message(STATUS "[lv_port] defconfig: ${LV_BUILD_DEFCONFIG_PATH}")

add_subdirectory(lvgl)
add_subdirectory(ports)
add_subdirectory(third_party)
add_subdirectory(app)

# 3D truck demo（工程带 demos 时才编）
if(CONFIG_LV_USE_DEMO_TRUCK AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/demos/lv_demo_truck/CMakeLists.txt)
    add_subdirectory(demos/lv_demo_truck)
endif()

if(SAFE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "[summary] 人脸后端=${SAFE_FACE_BACKEND} MQTT=${SAFE_FEATURE_MQTT} JSON=${SAFE_FEATURE_JSON} 测试=${SAFE_BUILD_TESTS}")
message(STATUS "[summary] 数据目录=${SAFE_DATA_DIR}")
