# 单元测试：只链业务库，不碰 LVGL。板子交叉编译时 SAFE_BUILD_TESTS 默认 OFF。

# safe_core 与 safe_hal 之间存在静态库级互相引用：core->hal（hal_time、
# face_service_module_users）与 hal->core（face_service.c 的 event_bus_post）。
# 单趟归档扫描无法闭合 —— 对测试目标用 --start-group 允许重扫，避免依赖「某个
# 恰好被引用的符号把对方 .o 拽进来」这种脆弱巧合（例：去掉 cred_reconcile 里一条
# 无订阅者的 event_bus_publish 后，event_bus.o 不再被拉入，链接即断）。
set(SAFE_TEST_LIBS
    -Wl,--start-group
    safe_core safe_hal safe_thirdparty
    -Wl,--end-group
    Threads::Threads)

macro(safe_add_test name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_BINARY_DIR}/generated/app)
    target_link_libraries(${name} PRIVATE ${SAFE_TEST_LIBS})
    add_test(NAME ${name} COMMAND ${name})
endmacro()

safe_add_test(test_totp)
safe_add_test(test_auth_fsm)
safe_add_test(test_cred_reconcile)
safe_add_test(test_event_bus)
safe_add_test(test_fm225_proto)
# FM225 后端「录入会话独占 + 静默无应答兜底」状态机测试。
# 用 posix_openpt 造 pty 假串口直接驱动真实后端（不需要真模组、不需要 socat）。
safe_add_test(test_fm225_enroll)
safe_add_test(test_store)
# 解锁后端：PIN 精确/虚位（FR-18）、管理员二次确认边界（§5.5）、
# 临时授权过期与次数耗尽（FR-9）、动态码与防重放（FR-3）。
safe_add_test(test_unlock_backend)
# 崩溃一致性自测（R1）：循环写盘期间 kill -9，校验 users.json 可解析且非旧即新。
# 默认 20 轮，长跑用 tools/crash_consistency_test.sh <更大轮次>。
safe_add_test(test_store_crash)
# 远程指令轻量访问控制：req_id 去重（防重放）、请求时效窗口（±120s）、命令分级。
# 纯逻辑（rpc_guard），不启 broker / 不依赖 paho。
safe_add_test(test_rpc)

# 分层检查：core 不碰 LVGL/平台头，hal 头文件不带平台实现，ui 不碰底层协议
find_program(SAFE_BASH bash)
if(SAFE_BASH)
    add_test(NAME check_layers
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tools/check_layers.sh ${CMAKE_SOURCE_DIR})
endif()
