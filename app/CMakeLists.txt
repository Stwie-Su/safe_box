# 应用层聚合：core / hal / ui / platform 各自成静态库，最终链接为 lvglsim。
# 业务库（safe_core / safe_hal）不依赖 LVGL，单元测试可以直接链接。

add_subdirectory(core)
add_subdirectory(hal)
add_subdirectory(ui)
add_subdirectory(platform)

add_executable(lvglsim
    main.c
    app.c)

# 版本头由构建阶段生成（cmake/SafeVersion.cmake），core/rpc.c 与 main.c 都包含它：
# 必须显式声明依赖，否则可能先编译、后生成头文件，拿到的是上一次的内容。
add_dependencies(safe_core version_header)
add_dependencies(lvglsim version_header)

target_include_directories(lvglsim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_BINARY_DIR}/generated/app)

# ★ core 与 hal 是静态库级互相引用（hal->core: net_scan_parse / event_bus；
#   core->hal: hal_time 等）。单趟归档扫描无法闭合，依赖「某个恰好被引用的
#   符号把对方 .o 拽进来」是脆弱巧合（tests/CMakeLists.txt 同款注释的教训：
#   曾因去掉一条无订阅者的 event_bus_publish 而链接即断）。用 --start-group
#   允许链接器重扫，lvgl_linux 是单向依赖留在组外。
target_link_libraries(lvglsim PRIVATE
    -Wl,--start-group
    safe_platform
    safe_ui
    safe_core
    safe_hal
    safe_thirdparty
    -Wl,--end-group
    lvgl_linux
    ${SAFE_EXTRA_LIBS}
    Threads::Threads)

if(WERROR)
    target_compile_options(lvglsim PRIVATE -Werror)
endif()

# 快捷运行：make run
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/lvglsim DEPENDS lvglsim)
