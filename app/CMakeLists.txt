# 应用层聚合：core / hal / ui / platform 各自成静态库，最终链接为 lvglsim。
# 业务库（safe_core / safe_hal）不依赖 LVGL，单元测试可以直接链接。

add_subdirectory(core)
add_subdirectory(hal)
add_subdirectory(ui)
add_subdirectory(platform)

add_executable(lvglsim
    main.c
    app.c)

target_include_directories(lvglsim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_BINARY_DIR}/generated/app)

target_link_libraries(lvglsim PRIVATE
    safe_platform
    safe_ui
    safe_core
    safe_hal
    lvgl_linux
    ${SAFE_EXTRA_LIBS}
    Threads::Threads)

if(WERROR)
    target_compile_options(lvglsim PRIVATE -Werror)
endif()

# 快捷运行：make run
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/lvglsim DEPENDS lvglsim)
