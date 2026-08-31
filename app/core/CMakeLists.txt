# 业务核心层：平台无关，禁止包含 LVGL 与平台头文件（tools/check_layers.sh 校验）。

set(SAFE_CORE_SRC
    err.c
    event_bus.c
    config.c
    store/store.c
    store/credentials.c
    auth/auth_fsm.c
    auth/totp.c
    auth/unlock_backend.c
    support/worker.c
    support/async_store.c
    support/crypto.c
    support/sha1.c)

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

target_link_libraries(safe_core PUBLIC safe_thirdparty Threads::Threads)

if(WERROR)
    target_compile_options(safe_core PRIVATE -Werror)
endif()
