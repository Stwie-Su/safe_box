# 通用编译选项

option(WERROR "把告警当作错误" OFF)
option(ASAN  "开启 Address/Undefined Sanitizer" OFF)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(ASAN)
    message(STATUS "[sanitizer] 已开启 ASan + UBSan")
    add_compile_options(-fsanitize=address,undefined,null)
    add_link_options(-fsanitize=address,undefined,null,leak)
endif()
