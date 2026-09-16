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
# 交叉编译时默认关闭需要外部库的特性，避免误链 host 库
if(CMAKE_CROSSCOMPILING)
    set(_mqtt_default  OFF)
    set(_tests_default OFF)
else()
    set(_mqtt_default  ON)
    set(_tests_default ON)
endif()

option(SAFE_FEATURE_MQTT "MQTT 远程通道（需要 paho-mqtt3a + cJSON）" ${_mqtt_default})
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

# Paho MQTT C
if(SAFE_FEATURE_MQTT)
    find_path(SAFE_PAHO_INCLUDE_DIR NAMES MQTTClient.h)
    find_library(SAFE_PAHO_LIBRARY NAMES paho-mqtt3a)
    if(SAFE_PAHO_INCLUDE_DIR AND SAFE_PAHO_LIBRARY)
        set(SAFE_HAVE_PAHO TRUE)
        list(APPEND SAFE_EXTRA_LIBS ${SAFE_PAHO_LIBRARY})
    else()
        set(SAFE_HAVE_PAHO FALSE)
        message(STATUS "[feature] paho-mqtt3a 未找到，MQTT 通道降级为空实现")
    endif()
else()
    set(SAFE_HAVE_PAHO FALSE)
endif()

# MQTT 依赖 cJSON 做报文解析，缺一不可
if(SAFE_FEATURE_MQTT AND NOT SAFE_HAVE_CJSON)
    message(STATUS "[feature] 缺少 cJSON，MQTT 通道一并降级为空实现")
    set(SAFE_HAVE_PAHO FALSE)
endif()

# 依赖头文件目录汇总，供 app 目标使用
set(SAFE_DEP_INCLUDE_DIRS "")
if(SAFE_HAVE_CJSON)
    list(APPEND SAFE_DEP_INCLUDE_DIRS ${SAFE_CJSON_INCLUDE_DIR})
endif()
if(SAFE_HAVE_PAHO)
    list(APPEND SAFE_DEP_INCLUDE_DIRS ${SAFE_PAHO_INCLUDE_DIR})
endif()
