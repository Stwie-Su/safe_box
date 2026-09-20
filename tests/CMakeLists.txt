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
safe_add_test(test_store_p0)
# 远程指令轻量访问控制：req_id 去重（防重放）、请求时效窗口（±120s）、命令分级。
# 纯逻辑（rpc_guard），不启 broker / 不依赖 paho。
safe_add_test(test_rpc)
# R9 自研 MQTT 客户端 T01：协议编解码（变长剩余长度边界与畸形、组包→解析
# round-trip、**半包与粘包**、缓冲不足与非法输入）。纯逻辑，不需要 broker。
safe_add_test(test_mqtt_codec)
# R9 T02：未确认报文队列 —— packet id 回绕、**存字节而非指针**、DUP 置位、
# 重传次数用尽后丢弃、队列满与非法输入。
safe_add_test(test_mqtt_inflight)
# R9 T03：连接状态机 —— 启动序列 / CONNACK(≠0) 不重连 / ping 超时判半开 /
# 退避序列（固定种子 → 精确值）/ 重传用尽丢弃 / 重连清空 inflight / 背压不回 PUBACK。
safe_add_test(test_mqtt_fsm)
# 只读用户表快照缓存（store_cache，双缓冲）：读侧读不到半张表、stamp 语义、钳位。
safe_add_test(test_store_cache)

# 分层检查：core 不碰 LVGL/平台头，hal 头文件不带平台实现，ui 不碰底层协议
find_program(SAFE_BASH bash)
if(SAFE_BASH)
    add_test(NAME check_layers
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tools/check_layers.sh ${CMAKE_SOURCE_DIR})

    # 主线程直连 store 写接口门禁（QA-20 防回归）。
    # 这类问题**改完还会再犯**（QA-20 就是主线程经 dispatch 直调 user_add 等
    # 写接口，破坏「文件 IO 由 worker 单线程持有」不变式），靠人记不住，
    # 必须有机械检查。豁免规则见脚本内注释（_worker / rpc_job_fn /
    # user_del_cascade / 行内 GATE-EXEMPT 标记 / auth_fsm 已知遗留）。
    add_test(NAME check_main_thread_store
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tools/check_main_thread_store.sh
                     ${CMAKE_SOURCE_DIR})
endif()

# ---- Sanitizer 验证（**不默认开启**）----
# 默认构建不带 Sanitizer（会拖慢并影响板子交叉编译）。要验证内存类问题请显式：
#   cmake -B /tmp/asan_build -DASAN=ON -DLV_PORT_DEFCONFIG=configs/pc.defconfig
#   cmake --build /tmp/asan_build -j4 && ctest --test-dir /tmp/asan_build
# 这条很重要：tests/test_store.c 早就覆盖了 QA-02 的 UAF 场景，却在默认 ctest 下
# **全绿通过** —— 仅仅因为没开 ASan。今后凡是内存类修复，都应在这套构建下再验一遍。
