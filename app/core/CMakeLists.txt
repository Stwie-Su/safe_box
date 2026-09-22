# 业务核心层：平台无关，禁止包含 LVGL 与平台头文件（tools/check_layers.sh 校验）。

set(SAFE_CORE_SRC
    err.c
    event_bus.c
    config.c
    store/store.c
    # 只读用户表快照缓存（双缓冲：worker 写 / 主线程读）。
    # QA-20 不变式升级为「读写全归 worker」之后，主线程读用户表的唯一合法通道。
    # 无第三方依赖，无条件编入。
    store/store_cache.c
    auth/auth_fsm.c
    auth/cred_reconcile.c
    auth/totp.c
    auth/unlock_backend.c
    support/worker.c
    support/async_store.c
    support/crypto.c
    support/sha1.c
    # 远程指令的轻量访问控制（req_id 去重 / 时效 / 命令分级）。
    # 纯逻辑、无第三方依赖，故**无条件**编入 core，便于单测直接链接。
    remote/rpc_guard.c
    # R9 自研 MQTT 客户端 T01：协议编解码层。纯函数（不 IO / 不取时间 / 不申请堆内存），
    # 无第三方依赖，故与 rpc_guard 一样**无条件**编入。
    remote/mqtt_codec.c
    # R9 T02：未确认报文队列（QoS1 重传用）。同为纯逻辑、无依赖，无条件编入。
    remote/mqtt_inflight.c
    # R9 T03：连接状态机（纯逻辑，时间由参数注入）。不碰套接字/线程，无条件编入。

    # 远程通道：依赖齐了编真实现，缺了编空实现，调用方代码不变。
    # ★ R9 T04 的关键改动：前置条件从「有第三方 MQTT 库 + cJSON」改为**只要 cJSON**。
    #   原因是板子上没有可用的 ARM 版第三方 MQTT 库，旧条件下板子必然走到 stub 分支
    #   → 板上远程通道永久断线。自研实现只依赖 libc + cJSON，板子与 PC 编同一份代码。
    remote/mqtt_fsm.c)

if(SAFE_HAVE_CJSON)
    list(APPEND SAFE_CORE_SRC remote/mqtt_client.c remote/rpc.c remote/tls_stream.c)
else()
    list(APPEND SAFE_CORE_SRC remote/mqtt_stub.c remote/rpc_stub.c)
endif()

add_library(safe_core STATIC ${SAFE_CORE_SRC})

target_include_directories(safe_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..                 # app/
    ${CMAKE_BINARY_DIR}/generated/app)           # app_version.h

target_include_directories(safe_core PRIVATE ${SAFE_DEP_INCLUDE_DIRS})

target_link_libraries(safe_core PUBLIC safe_hal safe_thirdparty Threads::Threads
                                       ${SAFE_MBEDTLS_LIBS})   # T06: mbedTLS（未启用时为空）

if(WERROR)
    target_compile_options(safe_core PRIVATE -Werror)
endif()
