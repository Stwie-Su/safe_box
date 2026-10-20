# ============================================================
#  版本头 app_version.h：git 描述必须跟着 HEAD 走
# ------------------------------------------------------------
#  ★ 为什么不能只用 configure_file（D36）
#    configure_file 是"配置期"动作，只在 cmake 重新 configure 时执行一次。
#    之后每次 cmake --build 都是纯构建，头文件不会重写 —— 版本串就冻结在
#    上一次 configure 的时刻。实测踩过：部署上去的二进制明明是 HEAD 构建，
#    日志却自称四天前的提交，排障时按版本号对不上代码。
#
#  ★ 做法
#    configure 阶段先跑一次（保证头文件存在），同时登记一个"每次构建都执行"的
#    自定义目标：它调 GenVersion.cmake 重新算 git 描述，**内容变了才写文件**。
#    于是：git 描述变了 → 头文件更新 → 相关文件重编；
#          只是重新构建 → 头文件不动 → 不触发任何重编译。
#
#  ★ 编译顺序
#    version_header 这个目标只负责"先生成头文件"，编译目标必须依赖它，
#    否则可能先编译后生成。依赖声明在 app/CMakeLists.txt（那里才拿得到目标名）。
# ============================================================

set(SAFE_VER_SRC    "${CMAKE_CURRENT_LIST_DIR}/app_version.h.in")
set(SAFE_VER_OUT    "${CMAKE_BINARY_DIR}/generated/app/app_version.h")
set(SAFE_VER_REPO   "${CMAKE_SOURCE_DIR}")
set(SAFE_VER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/GenVersion.cmake")

# configure 阶段跑一次：IDE 补全与首次编译都需要头文件先存在
include(${SAFE_VER_SCRIPT})
message(STATUS "[version] ${PROJECT_VERSION} (${SAFE_GIT_DESC})")

# 构建阶段每次跑：git 描述随 HEAD 变化，不能只在 configure 时固化
add_custom_target(version_header
    COMMAND ${CMAKE_COMMAND}
        -DSAFE_VER_SRC=${SAFE_VER_SRC}
        -DSAFE_VER_OUT=${SAFE_VER_OUT}
        -DSAFE_VER_REPO=${SAFE_VER_REPO}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        -DSAFE_DATA_DIR=${SAFE_DATA_DIR}
        -DSAFE_FACE_BACKEND=${SAFE_FACE_BACKEND}
        -P ${SAFE_VER_SCRIPT}
    COMMENT "刷新 app_version.h（git 描述）"
    VERBATIM)
