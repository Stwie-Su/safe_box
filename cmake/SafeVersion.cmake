# 生成 app_version.h：版本号、git 描述、构建时间、数据目录

set(SAFE_GIT_DESC "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SAFE_GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT SAFE_GIT_DESC)
    set(SAFE_GIT_DESC "v${PROJECT_VERSION}")
endif()

string(TIMESTAMP SAFE_BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/app_version.h.in
    ${CMAKE_BINARY_DIR}/generated/app/app_version.h
    @ONLY)

message(STATUS "[version] ${PROJECT_VERSION} (${SAFE_GIT_DESC})")
