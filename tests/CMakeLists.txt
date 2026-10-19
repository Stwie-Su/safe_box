# 单元测试：只链业务库，不碰 LVGL。板子交叉编译时 SAFE_BUILD_TESTS 默认 OFF。

# safe_core 与 safe_hal 之间存在静态库级互相引用：core->hal（hal_time、
# hal_face_module_users）与 hal->core（face_service.c 的 event_bus_post）。
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
safe_add_test(test_auth_verify)
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
# WLAN 后端命令应答解析：SSID 含空格 / \r\n / 容量截断 / 畸形行作废 /
# STATUS 取值与前缀不误命中。纯逻辑，不需要网卡与 wpa_supplicant。
safe_add_test(test_net_parse)
# OTA 状态契约（D19 回归）：追加式/重写式取最后匹配、截断、CRLF 等
safe_add_test(test_ota_status)
# 修改用户写通道（FR-9 v1.11 修改入口）：改名/改 PIN/改角色/改限制一次提交；
# 末位管理员不可降级、上限≤已用次数被拒、临时改非临时清零、被拒时零落盘。
safe_add_test(test_modify_user)

# QA 补缺（2026-10-06 验收核对）：三个此前零覆盖的模块。
# rpc_guard 的「提权」子集：needs_otp 白名单 + sync_time 时钟跳变门控。纯逻辑。
safe_add_test(test_rpc_escalation)
# worker：后台单线程执行 / 主线程派发 done / shutdown 不丢作业 的线程契约。
safe_add_test(test_worker)
# 密码学原语：SHA-1 / HMAC-SHA1（RFC 3174 / RFC 2202 向量）+ PBKDF2 向量 + 认证加密层。
safe_add_test(test_crypto)

# 配置校验（生产级补齐）：范围 / 路径 / 组合约束 / 分级（FATAL vs WARN）/ 钳制幂等。
# 纯函数测试，不读环境变量、不启动 store；路径项用 mkdtemp 造临时目录。
safe_add_test(test_config_validate)

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

    # RPC 指令清单一致性：rpc.c::dispatch() 的 strcmp 分支 <-> rpc_guard 的
    # 「已实现指令表」。同一事实散在两处，靠人记不住（D20 / D22b / D25 同源教训），
    # 必须机械比对；其中「自带补码流程」那条漏登记的失败方向是**不安全**的
    # （remote_unlock 本地弹码页 / sync_time 常规校时静默失效）。
    add_test(NAME check_rpc_cmd_table
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tools/check_rpc_cmd_table.sh
                     ${CMAKE_SOURCE_DIR})

    # supervisor 进程级契约（生产级补齐）：正常退出不重启 / 崩溃重启 /
    # 超限进安全态 / SIGTERM 转发 / 单实例锁 / daemon 模式 / 日志轮转。
    # 这些行为只存在于 fork/exec/waitpid/signal 的内核语义里，必须真起进程；
    # 脚本自带编译（cc 缺失时返回 77 → ctest 记为 Skipped）。
    add_test(NAME test_supervisor
             COMMAND ${SAFE_BASH} ${CMAKE_SOURCE_DIR}/tests/test_supervisor.sh)
    set_tests_properties(test_supervisor PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

    # 无头端到端（FR-11/FR-12）：起临时 broker + PC 应用，验「连上 → 下发指令 →
    # 回执关联 → kill -9 后 LWT」这条最小闭环。这是 §10 里唯一一条此前没有
    # 自动化载体的验收项（判据只在 R9 文档里，未脚本化）。
    # 用明文 11883：本用例验的是通道闭环，不是加密（加密由 FR-25 实机验收负责）。
    add_test(NAME e2e_headless
             COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/e2e_headless.py --app $<TARGET_FILE:lvglsim>)
    set_tests_properties(e2e_headless PROPERTIES TIMEOUT 180)

    # FR-29 OTA 期① 无头验收（需求 §10 集成层）：自带支持 Range/206 的 HTTP server，
    # 在临时目录里跑完整 check → apply 链路，覆盖 正常升级 / 摘要错 / 签名错 /
    # 剥 hmac 字段 / 版本不符（均要求拒绝且不留残余）/ 断点续传 / kill -9 断电续传。
    # 此前 OTA 只在真机上手测过一次（reports/device.log），**无可复跑载体**——
    # 脚本把板端路径写死后，PC 上跑就会往真实系统路径写。加路径 env 覆盖后才可能有
    # 这条用例；它因此也是「板端默认行为未变」的回归闸门。
    add_test(NAME ota_acceptance
             COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ota_acceptance.py)
    set_tests_properties(ota_acceptance PROPERTIES TIMEOUT 600)
endif()

# ---- Sanitizer 验证（**不默认开启**）----
# 默认构建不带 Sanitizer（会拖慢并影响板子交叉编译）。要验证内存类问题请显式：
#   cmake -B /tmp/asan_build -DASAN=ON -DLV_PORT_DEFCONFIG=configs/pc.defconfig
#   cmake --build /tmp/asan_build -j4 && ctest --test-dir /tmp/asan_build
# 这条很重要：tests/test_store.c 早就覆盖了 QA-02 的 UAF 场景，却在默认 ctest 下
# **全绿通过** —— 仅仅因为没开 ASan。今后凡是内存类修复，都应在这套构建下再验一遍。
