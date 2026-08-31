# LVGL 官方 Linux 移植层（原 src/lib）：显示/输入后端注册与初始化。
# 编译哪些后端由 Kconfig 决定，其余后端不进产物。

set(LV_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/lv_port)

file(GLOB LV_PORT_SRC CONFIGURE_DEPENDS ${LV_PORT_DIR}/*.c)
set(DISPLAY_BACKENDS ${LV_PORT_DIR}/display_backends)
set(INDEV_BACKENDS   ${LV_PORT_DIR}/indev_backends)

function(lv_linux_add_backend_if option dir file)
    if(${option})
        set(LV_PORT_BACKEND_SRC ${LV_PORT_BACKEND_SRC} ${dir}/${file} PARENT_SCOPE)
    endif()
endfunction()

set(LV_PORT_BACKEND_SRC "")
lv_linux_add_backend_if(CONFIG_LV_USE_WAYLAND     ${DISPLAY_BACKENDS} wayland.c)
lv_linux_add_backend_if(CONFIG_LV_USE_LINUX_DRM   ${DISPLAY_BACKENDS} drm.c)
lv_linux_add_backend_if(CONFIG_LV_USE_LINUX_FBDEV ${DISPLAY_BACKENDS} fbdev.c)
lv_linux_add_backend_if(CONFIG_LV_USE_SDL         ${DISPLAY_BACKENDS} sdl.c)
lv_linux_add_backend_if(CONFIG_LV_USE_X11         ${DISPLAY_BACKENDS} x11.c)
lv_linux_add_backend_if(CONFIG_LV_USE_GLFW        ${DISPLAY_BACKENDS} glfw3.c)
lv_linux_add_backend_if(CONFIG_LV_USE_EVDEV       ${INDEV_BACKENDS}   evdev.c)

add_library(lvgl_linux ${LV_PORT_SRC} ${LV_PORT_BACKEND_SRC})

target_include_directories(lvgl_linux PUBLIC ${LV_PORT_DIR})

target_link_libraries(lvgl_linux PUBLIC lvgl::lvgl)

# LVGL 的 Freetype include 不会自动传播到本目标，交叉编译时需要显式补上
if(LV_USE_FREETYPE)
    target_include_directories(lvgl_linux PRIVATE
        ${FREETYPE_INCLUDE_DIR_ft2build}
        ${FREETYPE_INCLUDE_DIR_freetype2})
endif()

install(DIRECTORY ${LV_PORT_DIR}/
    DESTINATION include/lvgl
    FILES_MATCHING
    PATTERN "backends*" EXCLUDE
    PATTERN "*.h")

install(TARGETS lvgl_linux
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
