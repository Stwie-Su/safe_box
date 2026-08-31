# 硬件抽象层：接口 + 可替换后端。
# 人脸后端三个实现全部编入（都很小），默认后端由 SAFE_FACE_BACKEND 在运行期决定；
# 想彻底不编某个后端时，从下面的列表删掉对应 .c 即可。

set(SAFE_HAL_SRC
    face/face_service.c
    face/backend_fake.c
    face/backend_fm225.c
    face/backend_none.c
    time/time_service.c
    time/time_sys.c
    actuator/actuator_service.c
    actuator/actuator_mock.c
    camera/camera_service.c
    camera/camera_null.c)

add_library(safe_hal STATIC ${SAFE_HAL_SRC})

target_include_directories(safe_hal PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..                 # app/
    ${CMAKE_BINARY_DIR}/generated/app)

target_link_libraries(safe_hal PUBLIC safe_thirdparty Threads::Threads)

if(WERROR)
    target_compile_options(safe_hal PRIVATE -Werror)
endif()
