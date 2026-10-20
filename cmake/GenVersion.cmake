# 生成 app_version.h。
#
# 两种跑法都要支持：
#   1) configure 阶段：SafeVersion.cmake 里 include() 本文件 —— 保证 configure 完
#      立刻就有头文件（IDE 补全、首次编译都不会找不到）。
#   2) 构建阶段：cmake -P 本文件 —— 保证 git 描述跟着 HEAD 走，而不是冻结在
#      上一次 configure 的时刻（D36：二进制是 HEAD 构建，却自称四天前的版本）。
#
# 入参（-D 或上层 set 均可）：
#   SAFE_VER_SRC / SAFE_VER_OUT / SAFE_VER_REPO
#   PROJECT_VERSION / PROJECT_VERSION_MAJOR / PROJECT_VERSION_MINOR / PROJECT_VERSION_PATCH
#   SAFE_DATA_DIR / SAFE_FACE_BACKEND

find_package(Git QUIET)

set(SAFE_GIT_DESC "")
if(GIT_FOUND AND EXISTS "${SAFE_VER_REPO}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${SAFE_VER_REPO}
        OUTPUT_VARIABLE SAFE_GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT SAFE_GIT_DESC)
    set(SAFE_GIT_DESC "v${PROJECT_VERSION}")
endif()

string(TIMESTAMP SAFE_BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)

configure_file("${SAFE_VER_SRC}" "${SAFE_VER_OUT}.tmp" @ONLY)

# 内容没变就不覆盖。
# ★ 为什么必须比一次再写：SAFE_BUILD_TIME 每次构建都不同，若照写，这个头文件
#   就变成"每次构建都变"，所有包含它的翻译单元（core/rpc.c、main.c）都会重编，
#   增量构建等于失效。比较时先把时间戳那一行抹平，只认"其余内容变没变"——
#   真正需要跟着代码走的是 git 描述，时间戳只是附带的现场信息。
file(READ "${SAFE_VER_OUT}.tmp" _new)
set(_cmp_new "${_new}")
if(EXISTS "${SAFE_VER_OUT}")
    file(READ "${SAFE_VER_OUT}" _old)
    string(REGEX REPLACE "SAFE_BUILD_TIME \"[^\"]*\"" "SAFE_BUILD_TIME \"<ts>\"" _cmp_new "${_new}")
    string(REGEX REPLACE "SAFE_BUILD_TIME \"[^\"]*\"" "SAFE_BUILD_TIME \"<ts>\"" _cmp_old "${_old}")
    if(_cmp_old STREQUAL _cmp_new)
        file(REMOVE "${SAFE_VER_OUT}.tmp")
        return()
    endif()
endif()

file(RENAME "${SAFE_VER_OUT}.tmp" "${SAFE_VER_OUT}")
