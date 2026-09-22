# ============================================================
#  可选特性与依赖探测
# ------------------------------------------------------------
#  原则：核心链路（UI + 认证 + 存储）永远可编译；
#  外部依赖缺失时对应特性降级为空实现，而不是让构建失败。
# ============================================================

# ---------------- 人脸后端 ----------------
set(SAFE_FACE_BACKEND "fm225" CACHE STRING "人脸后端：fake(模拟) / fm225(真实模组) / none(停用)")
set_property(CACHE SAFE_FACE_BACKEND PROPERTY STRINGS fake fm225 none)

# ---------------- 特性开关 ----------------
# 交叉编译时默认关闭需要外部库的特性，避免误链 host 库。
# ★ 例外（R9 起）：MQTT 已改为**自研**，不再依赖任何第三方 MQTT 库，
#   唯一前置条件只剩 cJSON。旧的「交叉编译默认关 MQTT」正是当年为绕开
#   「板子没有 ARM 版第三方 MQTT 库」而设 —— 该前提已不存在；若继续默认关，
#   板子会静默编进 mqtt_stub.c，远程通道**永久断线**，R9 的全部收益落空。
#   故 MQTT 一律默认 ON；到底能不能编真实现，由下面的 SAFE_HAVE_CJSON 决定。
# （测试仍默认关：ctest 跑的是 host 二进制，交叉产物在本机执行不了。）
if(CMAKE_CROSSCOMPILING)
    set(_mqtt_default  ON)
    set(_tests_default OFF)
else()
    set(_mqtt_default  ON)
    set(_tests_default ON)
endif()

option(SAFE_FEATURE_MQTT "MQTT 远程通道（自研实现，需要 cJSON）" ${_mqtt_default})
option(SAFE_FEATURE_JSON "JSON 存储后端（需要 cJSON）" ON)
option(SAFE_BUILD_TESTS  "构建单元测试" ${_tests_default})
option(SAFE_DEBUG_HOOKS "板子构建也编译调试钩子(切页/截图/开弹窗)，仅验证固件用" OFF)

# ---------------- 运行时数据目录 ----------------
if(CMAKE_CROSSCOMPILING)
    set(_data_default "/var/lib/safe")
else()
    set(_data_default "${CMAKE_SOURCE_DIR}/data")
endif()
set(SAFE_DATA_DIR "${_data_default}" CACHE PATH "运行时数据目录")

# ---------------- 依赖探测 ----------------
set(SAFE_EXTRA_LIBS "")

# cJSON
if(SAFE_FEATURE_JSON)
    find_path(SAFE_CJSON_INCLUDE_DIR NAMES cJSON.h PATH_SUFFIXES cjson)
    find_library(SAFE_CJSON_LIBRARY NAMES cjson)
    if(SAFE_CJSON_INCLUDE_DIR AND SAFE_CJSON_LIBRARY)
        set(SAFE_HAVE_CJSON TRUE)
        list(APPEND SAFE_EXTRA_LIBS ${SAFE_CJSON_LIBRARY})
    else()
        set(SAFE_HAVE_CJSON FALSE)
        message(STATUS "[feature] cJSON 未找到，JSON 存储后端降级为受限模式")
    endif()
else()
    set(SAFE_HAVE_CJSON FALSE)
endif()

# ---------------- MQTT over TLS（R9 T06：mbedTLS） ----------------
# ★ ARM 前置（2026-09-22 实测）：板子 Buildroot sysroot 里**既没有 mbedTLS 也没有 cJSON**，
#   必须先交叉编译装进去（静态库 —— 板子无需额外部署 .so）：
#     BR=/home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host
#     SYSROOT=$BR/arm-buildroot-linux-gnueabihf/sysroot
#     export PATH=$BR/bin:$PATH
#     # mbedTLS 2.28.8（源码需自备：VM 无外网，从宿主机传）
#     make -j4 lib CC=arm-buildroot-linux-gnueabihf-gcc \
#                  AR=arm-buildroot-linux-gnueabihf-ar \
#                  CFLAGS="-O2 --sysroot=$SYSROOT"
#     cp include/mbedtls/*.h $SYSROOT/usr/include/mbedtls/
#     cp -r include/psa $SYSROOT/usr/include/
#     cp library/libmbedcrypto.a library/libmbedtls.a library/libmbedx509.a $SYSROOT/usr/lib/
#     # cJSON 1.7.18（MQTT 与 JSON 存储后端的**共同**前置；没有它板子会退化成 mqtt_stub）
#     arm-buildroot-linux-gnueabihf-gcc -c cJSON.c -I. -O2 -fPIC --sysroot=$SYSROOT -o cJSON.o
#     arm-buildroot-linux-gnueabihf-ar rcs libcjson.a cJSON.o
#     mkdir -p $SYSROOT/usr/include/cjson && cp cJSON.h $SYSROOT/usr/include/cjson/
#     cp libcjson.a $SYSROOT/usr/lib/
#   PC 侧只需：sudo apt install libmbedtls-dev libcjson-dev

# 传输层抽象（tls_stream）已按双后端写好：
#   后端 A = 明文 TCP（默认）；后端 B = mbedTLS（由本开关启用）。
# ★ 找不到 mbedTLS 时**直接 FATAL_ERROR**，绝不静默退化成明文 ——
#   「以为在加密其实是明文」比直接报错危险得多。
option(SAFE_FEATURE_MQTT_TLS "MQTT over TLS（需要 mbedTLS）" OFF)

set(SAFE_MBEDTLS_LIBS "")
if(SAFE_FEATURE_MQTT_TLS)
    find_path(SAFE_MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h)
    find_library(SAFE_MBEDTLS_LIBRARY    NAMES mbedtls)
    find_library(SAFE_MBEDX509_LIBRARY   NAMES mbedx509)
    find_library(SAFE_MBEDCRYPTO_LIBRARY NAMES mbedcrypto)
    if(NOT (SAFE_MBEDTLS_INCLUDE_DIR AND SAFE_MBEDTLS_LIBRARY AND
            SAFE_MBEDX509_LIBRARY AND SAFE_MBEDCRYPTO_LIBRARY))
        message(FATAL_ERROR
            "[feature] SAFE_FEATURE_MQTT_TLS=ON 但未找到 mbedTLS。"
            " PC: sudo apt install libmbedtls-dev；"
            " ARM: 需先交叉编译 mbedTLS 并指向其路径。")
    endif()
    set(SAFE_MBEDTLS_LIBS
        ${SAFE_MBEDTLS_LIBRARY} ${SAFE_MBEDX509_LIBRARY} ${SAFE_MBEDCRYPTO_LIBRARY})
    include_directories(${SAFE_MBEDTLS_INCLUDE_DIR})
    add_compile_definitions(SAFE_FEATURE_TLS)
    message(STATUS "[feature] mbedTLS 已启用: ${SAFE_MBEDTLS_LIBRARY}")
endif()

# ---------------- MQTT（R9 起自研，不再需要第三方 MQTT 库） ----------------
# 旧实现依赖第三方 MQTT 库，而板子上没有可用的 ARM 版 —— 交叉构建会静默退化成
# mqtt_stub.c，导致**板上远程通道永久断线**（这正是 R9 要解决的问题）。
# 自研实现只依赖 libc + cJSON，故 MQTT 的前置条件从「第三方库 + cJSON」降为「只要
# cJSON」；缺 cJSON 时才退化空实现。构建不再探测、也不再链接任何第三方 MQTT 库。
if(SAFE_FEATURE_MQTT AND NOT SAFE_HAVE_CJSON)
    message(STATUS "[feature] 缺少 cJSON，MQTT 通道降级为空实现")
endif()

# 依赖头文件目录汇总，供 app 目标使用
set(SAFE_DEP_INCLUDE_DIRS "")
if(SAFE_HAVE_CJSON)
    list(APPEND SAFE_DEP_INCLUDE_DIRS ${SAFE_CJSON_INCLUDE_DIR})
endif()
