# 平台引导层：PC 与板子的差异集中在这里，其余层不出现平台判断。

if(CONFIG_LV_USE_SDL)
    set(SAFE_PLATFORM_SRC platform_sdl.c)
elseif(CONFIG_LV_USE_LINUX_FBDEV)
    set(SAFE_PLATFORM_SRC platform_fbdev.c)
else()
    set(SAFE_PLATFORM_SRC platform_null.c)
endif()

add_library(safe_platform STATIC
    ${SAFE_PLATFORM_SRC}
    perf_probe.c
    debug_hooks.c)

target_include_directories(safe_platform PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..            # app/
    ${CMAKE_SOURCE_DIR}/ports/lv_port)      # LVGL 官方移植层头文件

target_include_directories(safe_platform PRIVATE ${CMAKE_BINARY_DIR}/generated)

# 平台能力宏由构建系统显式给出，不依赖 LVGL 内部宏向应用层的传播
if(CONFIG_LV_USE_SDL)
    target_compile_definitions(safe_platform PRIVATE SAFE_PLATFORM_PC=1)
endif()
if(CONFIG_LV_USE_SNAPSHOT)
    target_compile_definitions(safe_platform PRIVATE SAFE_HAVE_SNAPSHOT=1)
endif()

target_link_libraries(safe_platform PUBLIC safe_ui safe_core lvgl_linux)

if(WERROR)
    target_compile_options(safe_platform PRIVATE -Werror)
endif()

# 板子构建默认关闭调试钩子（切页/截图/开弹窗）；验证固件用 -DSAFE_DEBUG_HOOKS=ON 打开，
# 正式固件保持关闭，避免在生产镜像里留测试后门。
if(SAFE_DEBUG_HOOKS)
    target_compile_definitions(safe_platform PRIVATE SAFE_ENABLE_DEBUG_HOOKS=1)
    message(STATUS "[feature] 调试钩子已启用（板子验证固件）")
endif()
