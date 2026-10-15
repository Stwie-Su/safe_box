# 硬件抽象层：接口 + 可替换后端。
# 人脸后端三个实现全部编入（都很小），默认后端由 SAFE_FACE_BACKEND 在运行期决定；
# ★ backend_fake.c 此前漏编（下面列表里没有它，与这句注释自相矛盾），导致
#   face_backend_fake() 全工程零引用、查找表里补不上 fake —— SAFE_FACE_BACKEND=fake
#   与 RPC inject_face 这条调试/验证通道形同虚设。此处补回。
# 相机后端同理（SAFE_CAMERA_BACKEND 运行期环境变量，v4l2 初始化失败自动降级 null）；
# 想彻底不编某个后端时，从下面的列表删掉对应 .c 即可。

set(SAFE_HAL_SRC
    face/face_service.c
    face/face_thread.c
    face/fm225_proto.c
    face/backend_fake.c
    face/backend_fm225.c
    face/backend_none.c
    time/time_service.c
    time/backend_sys.c
    time/backend_rtc.c
    actuator/actuator_service.c
    actuator/backend_mock.c
    storage/storage_service.c
    camera/camera_service.c
    camera/backend_none.c
    camera/backend_v4l2.c
    # 看门狗：Linux 真狗（/dev/watchdog）+ none 占位 + 分发层。
    # 与相机/时间同构：SAFE_WATCHDOG_BACKEND 运行期选择，打不开设备自动降级 null，
    # 因此 PC（无 /dev/watchdog）与板子编同一份主循环代码。
    watchdog/watchdog_service.c
    watchdog/backend_linux.c
    watchdog/backend_none.c
    # WLAN：wpa_supplicant ctrl_iface 后端 + 降级后端 + 分发层。
    # 依赖只有 libc 与 POSIX socket，板子与 PC 编同一份代码（不引入任何第三方库）。
    net/net_service.c
    net/backend_wpa.c
    net/backend_none.c)

add_library(safe_hal STATIC ${SAFE_HAL_SRC})

target_include_directories(safe_hal PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..                 # app/
    ${CMAKE_BINARY_DIR}/generated/app)

target_link_libraries(safe_hal PUBLIC safe_thirdparty Threads::Threads)

if(WERROR)
    target_compile_options(safe_hal PRIVATE -Werror)
endif()

