# 视图层：只做渲染与事件绑定，判定在 core，硬件动作在 hal。
# 页面与组件用 GLOB + CONFIGURE_DEPENDS，新增文件后自动重新配置。

file(GLOB SAFE_UI_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/pages/*.c
    ${CMAKE_CURRENT_LIST_DIR}/widgets/*.c
    ${CMAKE_CURRENT_LIST_DIR}/fonts/*.c)

add_library(safe_ui STATIC ${SAFE_UI_SRC})

target_include_directories(safe_ui PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..            # app/
    ${CMAKE_CURRENT_LIST_DIR})              # app/ui

target_include_directories(safe_ui PRIVATE ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(safe_ui PUBLIC safe_core safe_hal lvgl::lvgl)

if(WERROR)
    target_compile_options(safe_ui PRIVATE -Werror)
endif()
