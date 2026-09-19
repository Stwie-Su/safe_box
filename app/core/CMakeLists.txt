# 业务核心层：平台无关，禁止包含 LVGL 与平台头文件（tools/check_layers.sh 校验）。

set(SAFE_CORE_SRC
    err.c
    event_bus.c
    config.c
    store/store.c
    auth/auth_fsm.c
    auth/cred_reconcile.c
    auth/totp.c
    auth/unlock_backend.c
    support/worker.c
    support/async_store.c
    support/crypto.c
    support/sha1.c
    # 远程指令的轻量访问控制（req_id 去重 / 时效 / 命令分级）。
    # 纯逻辑、无 paho/cJSON 依赖，故**无条件**编入 core，便于单测直接链接。
    remote/rpc_guard.c
    # R9 自研 MQTT 客户端 T01：协议编解码层。纯函数（不 IO / 不取时间 / 不 malloc），
    # 无 paho / cJSON 依赖，故与 rpc_guard 一样**无条件**编入 —— 这样即便在
    # Paho 缺失的构建下（当前板子正是如此）它也能继续编译并跑单测。
    remote/mqtt_codec.c
    # R9 T02：未确认报文队列（QoS1 重传用）。同为纯逻辑、无依赖，无条件编入。
    remote/mqtt_inflight.c)

# 远程通道：依赖齐了编真实现，缺了编空实现，调用方代码不变
if(SAFE_HAVE_PAHO AND SAFE_HAVE_CJSON)
    list(APPEND SAFE_CORE_SRC remote/mqtt_client.c remote/rpc.c)
else()
    list(APPEND SAFE_CORE_SRC remote/mqtt_stub.c remote/rpc_stub.c)
endif()

add_library(safe_core STATIC ${SAFE_CORE_SRC})

target_include_directories(safe_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..                 # app/
    ${CMAKE_BINARY_DIR}/generated/app)           # app_version.h

target_include_directories(safe_core PRIVATE ${SAFE_DEP_INCLUDE_DIRS})

target_link_libraries(safe_core PUBLIC safe_hal safe_thirdparty Threads::Threads)

if(WERROR)
    target_compile_options(safe_core PRIVATE -Werror)
endif()
