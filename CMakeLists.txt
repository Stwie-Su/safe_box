cmake_minimum_required(VERSION 3.11)
project(lvgl)

# ============================================================
#  lv_port_linux（LVGL v9）工程构建说明 —— PC 模拟 vs 开发板
# ------------------------------------------------------------
#  PC 与开发板的差异【不在本文件里】，而是由 Kconfig 的【defconfig】决定：
#   - configs/pc.defconfig           —— Ubuntu 桌面 SDL2 窗口（开发验证用，默认）
#   - configs/get_started.defconfig  —— i.MX6ULL 开发板 FBDEV+EVDEV（交叉编译上板用）
#  【不使用 .config】：通过下方 LV_PORT_DEFCONFIG 选择上面两套 defconfig。
#  两套 defconfig 仅【后端三行】不同，其余完全相同，业务代码零改动。
#  build_pc / build 只是【out-of-source 构建目录名】，配置差异全在 defconfig。
#  切换 PC<->板子：cmake -DLV_PORT_DEFCONFIG=... 重新配置即可，源码零改动。
# ============================================================

option(WERROR "Treat warnings as errors for LVGL targets" OFF)
option(ASAN "Enable Sanitizer Options" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)   # gcc 6.2.1 对 C++17 支持不全，降到 14（本项目纯 C，无影响）
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)   # LV_OS_PTHREAD + lib 后端需要 pthread（Buildroot glibc 2.28 需 -lpthread）

if(ASAN)
    message(STATUS "Enabling Sanitizer")
    add_compile_options(-fsanitize=address,undefined,null)
    add_link_options(-fsanitize=address,undefined,null,leak)
endif()

set(LV_BUILD_USE_KCONFIG ON CACHE BOOL "use kconfig" FORCE)
set(LV_BUILD_KCONFIG_ROOT "${CMAKE_CURRENT_LIST_DIR}/Kconfig" CACHE PATH "path to root kconfig" FORCE)

# ============================================================
#  不用 .config：通过 LV_PORT_DEFCONFIG 在【PC】与【开发板】两套 defconfig 间选择
#   - configs/pc.defconfig           : Ubuntu 桌面 SDL2 窗口（默认，开发验证用）
#   - configs/get_started.defconfig  : i.MX6ULL 开发板 FBDEV+EVDEV（交叉编译上板用）
#  切换开发板：cmake -DLV_PORT_DEFCONFIG=configs/get_started.defconfig ...
#  两套 defconfig 仅【后端三行】不同，业务代码零改动。
# ============================================================
set(LV_PORT_DEFCONFIG "configs/pc.defconfig" CACHE STRING
    "选择配置：configs/pc.defconfig(PC/SDL) 或 configs/get_started.defconfig(开发板/FBDEV+EVDEV)") #当你用 cmake -DLV_PORT_DEFCONFIG=configs/get_started.defconfig .. 执行后，这个新值会被写入 CMakeCache.txt。下一次再运行 cmake 时，即使不加 -D 参数，它仍然会使用上一次你指定的值
set(LV_BUILD_DEFCONFIG_PATH "${CMAKE_CURRENT_LIST_DIR}/${LV_PORT_DEFCONFIG}" CACHE PATH "path to defconfig" FORCE)
unset(LV_BUILD_DOTCONFIG_PATH)
message(STATUS "Using defconfig: ${LV_BUILD_DEFCONFIG_PATH}")

add_subdirectory(lvgl)

if(CONFIG_LV_USE_DEMO_TRUCK)
    add_subdirectory(demos/lv_demo_truck)
    set(EXTRA_LIBS lv_demo_truck_ui)
endif()

file(GLOB LV_LINUX_SRC src/lib/*.c)
set(LV_LINUX_INC src/lib)

# ===== 业务应用层源码（从 lv_port_pc_vscode 迁移，见移植记录）=====
# ui/pages + ui/fonts 用 GLOB，新增页面/字体后重新 Configure 即可
file(GLOB APP_UI_SOURCES CONFIGURE_DEPENDS
    src/ui/*.c
    src/ui/pages/*.c
    src/ui/fonts/*.c)
set(APP_CORE_SOURCES
    src/core/config.c
    src/core/store.c
    src/core/unlock_backend.c
    src/core/crypto.c
    src/core/worker.c
    src/core/async_store.c
    src/core/sha1.c
    src/core/totp.c
    src/core/auth_fsm.c
    src/core/rpc.c
    src/core/mqtt_client.c
    src/hal/actuator.c
    src/hal/hal_time.c
    src/hal/hal_face.c
    third_party/aes/aes.c
    third_party/sha256/sha256.c)
set(APP_SOURCES ${APP_UI_SOURCES} ${APP_CORE_SOURCES})
set(LV_LINUX_BACKEND_SRC)
set(DISPLAY_BACKENDS src/lib/display_backends)
set(INDEV_BACKENDS src/lib/indev_backends)

# —— 后端选择：是否编译某个后端 .c，完全由 Kconfig 变量（.config/defconfig）决定 ——
#   PC 模拟：CONFIG_LV_USE_SDL=y          -> 编译 sdl.c（桌面窗口 + 鼠标/键盘捕获）
#   开发板  ：CONFIG_LV_USE_LINUX_FBDEV=y + CONFIG_LV_USE_EVDEV=y -> 编译 fbdev.c + evdev.c
#   其它后端（WAYLAND/DRM/X11/GLFW）保持关闭即可，不编译进产物。
function(lv_linux_add_backend_if option dir file)
    if(${option})
        list(APPEND LV_LINUX_BACKEND_SRC ${dir}/${file})
        set(LV_LINUX_BACKEND_SRC ${LV_LINUX_BACKEND_SRC} PARENT_SCOPE)
    endif()
endfunction()

lv_linux_add_backend_if(CONFIG_LV_USE_WAYLAND     ${DISPLAY_BACKENDS} wayland.c)
lv_linux_add_backend_if(CONFIG_LV_USE_LINUX_DRM   ${DISPLAY_BACKENDS} drm.c)
lv_linux_add_backend_if(CONFIG_LV_USE_LINUX_FBDEV ${DISPLAY_BACKENDS} fbdev.c)
lv_linux_add_backend_if(CONFIG_LV_USE_SDL         ${DISPLAY_BACKENDS} sdl.c)
lv_linux_add_backend_if(CONFIG_LV_USE_X11         ${DISPLAY_BACKENDS} x11.c)
lv_linux_add_backend_if(CONFIG_LV_USE_GLFW        ${DISPLAY_BACKENDS} glfw3.c)
lv_linux_add_backend_if(CONFIG_LV_USE_EVDEV       ${INDEV_BACKENDS}   evdev.c)

# Library type is determined by BUILD_SHARED_LIBS (shared if ON, static if OFF)
add_library(lvgl_linux ${LV_LINUX_SRC} ${LV_LINUX_BACKEND_SRC})

set(LIBS lvgl::lvgl)

if(CONFIG_LV_BUILD_DEMOS)
list(APPEND LIBS lvgl::demos)
endif()

if(CONFIG_LV_BUILD_EXAMPLES)
list(APPEND LIBS lvgl::examples)
endif()

target_link_libraries(lvgl_linux PUBLIC ${LIBS})

# LVGL 的 Freetype include 没有自动传播到 lvgl_linux 目标（交叉编译时），
# 这里显式补上，否则 evdev.c 等包含 lv_freetype_private.h 的文件编译报 ft2build.h 找不到。
if(LV_USE_FREETYPE)
    # find_package 在 lvgl/ 子目录执行，FREETYPE_INCLUDE_DIRS 未传播到父目录；
    # 直接用 FindFreetype 写入 cache 的 ft2build / freetype2 头文件路径。
    target_include_directories(lvgl_linux PRIVATE
        ${FREETYPE_INCLUDE_DIR_ft2build}
        ${FREETYPE_INCLUDE_DIR_freetype2})
endif()
# —— 最终可执行文件 lvglsim：main.c + 业务 app 源码，链接 lvgl_linux + pthread ——
add_executable(lvglsim src/main.c ${APP_SOURCES})
target_include_directories(lvglsim PRIVATE
    ${CMAKE_SOURCE_DIR}/src            # "ui/ui.h" "core/*.h" "hal/*.h"
    ${CMAKE_SOURCE_DIR}/third_party/aes
    ${CMAKE_SOURCE_DIR}/third_party/sha256)

# 数据目录：PC 阶段用工程内 src/safe/（免权限）；板子用 /var/lib/safe/（DESIGN.md §4.1）
if(CONFIG_LV_USE_SDL)
    target_compile_definitions(lvglsim PRIVATE)
else()
    target_compile_definitions(lvglsim PRIVATE SAFE_DIR_DEVICE)
endif()
target_link_libraries(lvglsim PUBLIC lvgl_linux ${EXTRA_LIBS} Threads::Threads paho-mqtt3a cjson)

if(WERROR)
    target_compile_options(lvglsim PRIVATE -Werror)
    target_compile_options(lvgl PRIVATE -Werror)
    target_compile_options(lvgl_linux PRIVATE -Werror)
endif()


# Install the lvgl_linux library and its headers
install(DIRECTORY src/lib/
    DESTINATION include/lvgl
    FILES_MATCHING
    PATTERN "backends*" EXCLUDE
    PATTERN "*.h")

install(TARGETS lvgl_linux
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# 方便本地跑：make run 等价于执行 build/bin/lvglsim
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/lvglsim DEPENDS lvglsim)
