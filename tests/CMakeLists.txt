# 单元测试：只链业务库，不碰 LVGL。板子交叉编译时 SAFE_BUILD_TESTS 默认 OFF。

set(SAFE_TEST_LIBS safe_core safe_hal safe_thirdparty Threads::Threads)

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
safe_add_test(test_event_bus)
safe_add_test(test_fm225_proto)
safe_add_test(test_store)
# 崩溃一致性自测（R1）：循环写盘期间 kill -9，校验 users.json 可解析且非旧即新。
# 默认 20 轮，长跑用 tools/crash_consistency_test.sh <更大轮次>。
safe_add_test(test_store_crash)

# 分层检查：core 不碰 LVGL/平台头，hal 头文件不带平台实现，ui 不碰底层协议
find_program(SAFE_BASH bash)
if(SAFE_BASH)
    add_test(NAME check_layers
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tools/check_layers.sh ${CMAKE_SOURCE_DIR})
endif()
