# 交叉编译工具链：百问网 100ask SDK 自带 Buildroot 2020.02 工具链
#
# 为什么用 arm-buildroot-linux-gnueabihf-gcc 而非 Linaro 6.2.1：
#   1) LVGL v9 的 evdev(触摸) 后端依赖 libevdev，Buildroot sysroot 已带 ARM 版 libevdev；
#      Linaro 6.2.1 的 sysroot 没有 libevdev，链接会报 `cannot find -levdev`。
#   2) 与板子 rootfs 同源（同一个 Buildroot 2020.02 / 同一 glibc），ABI 完全一致，
#      链接、运行时不会有 glibc 版本不匹配问题。
#
# 用法（在 Ubuntu 服务器 192.168.150.139 上）：
#   cmake -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake
#   cmake --build build

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(BR_HOST /home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host)
set(SYSROOT  ${BR_HOST}/arm-buildroot-linux-gnueabihf/sysroot)

set(CMAKE_C_COMPILER   ${BR_HOST}/bin/arm-buildroot-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER ${BR_HOST}/bin/arm-buildroot-linux-gnueabihf-g++)

set(CMAKE_SYSROOT ${SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)    # 程序(python/pkg-config)用宿主机
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 让 pkg-config 找到交叉 sysroot 里的 libevdev（否则会找到宿主机 x86 版导致链接错误）
set(ENV{PKG_CONFIG_LIBDIR} "${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYSROOT}")
