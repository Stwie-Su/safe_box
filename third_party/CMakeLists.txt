# 自包含第三方代码（无外部依赖）

add_library(safe_thirdparty STATIC
    aes/aes.c
    sha256/sha256.c
    stb/stb_image_impl.c)

target_include_directories(safe_thirdparty PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/aes
    ${CMAKE_CURRENT_LIST_DIR}/sha256
    ${CMAKE_CURRENT_LIST_DIR}/stb)

# 第三方代码不套用本工程的告警等级
if(NOT MSVC)
    target_compile_options(safe_thirdparty PRIVATE -w)
endif()
