/**
 * @file app.c
 * 应用编排实现。
 *
 * 初始化顺序是有依赖的，改动前先看清链条：
 *   store（数据目录与用户表）→ worker（异步落盘）→ 执行器 → 配置快照
 *     → 界面（依赖主题与字体）→ 状态机（依赖存储里的用户与策略）→ 远程通道
 */

#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/auth/auth_fsm.h"
#include "core/auth/cred_reconcile.h"
#include "core/event_bus.h"
#include "core/remote/mqtt_client.h"
#include "core/remote/rpc.h"
#include "core/store/store.h"
#include "core/support/worker.h"
#include "hal/hal_actuator.h"
#include "hal/hal_camera.h"
#include "hal/face/face_thread.h"
#include "hal/hal_face.h"
#include "hal/hal_time.h"
/* 编排层唯一允许包含的时间内部头：只为调用 time_service_init()。 */
#include "hal/time/time_backend.h"
#include "platform/platform.h"
#include "ui/ui.h"
#include "app_version.h"

/* 认证结果同时进总线与远程通道：
 * 走总线是为了让 UI 顶栏、告警等后续订阅方不用再改状态机；
 * 走远程通道是因为 MQTT 上报的契约来自需求文档，独立实现更清楚。
 * 总线 payload 为 POD（evt/user/detail 定长数组，步骤 3a），栈变量即可。 */
static void on_auth_event(const char * evt, const char * user, const char * detail, int res)
{
    ev_auth_result_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.evt,    evt    ? evt    : "", sizeof(p.evt)    - 1);
    strncpy(p.user,   user   ? user   : "", sizeof(p.user)   - 1);
    strncpy(p.detail, detail ? detail : "", sizeof(p.detail) - 1);
    p.res = res;
    event_bus_publish(EV_AUTH_RESULT, &p);

    rpc_publish_event(evt, user, detail, res);
}

static void on_bus_auth_result(ev_topic_t topic, const void * payload, void * user)
{
    (void)topic;
    (void)user;
    const ev_auth_result_t * p = (const ev_auth_result_t *)payload;
    if(p == NULL) return;
    printf("[auth] %s user=%s res=%d detail=%s\n", p->evt, p->user, p->res, p->detail);
}

/* 用户 / 凭据变更 → 远程通道（safe/log）。
 *
 * 为什么用「订阅总线」而不是在 6 个 CRUD 点各调一次 rpc_publish_event：
 *   1) 变更点全在 worker 线程，而 MQTT 上报属于远程通道，把两者写在一处会让
 *      「本地改一个用户」和「发一条 MQTT」绑死 —— 断线时要么阻塞本地操作，
 *      要么每个点各写一遍重试逻辑。走总线后，上报只是**一个订阅者**：
 *      断线/队列满时它自己丢，本地变更照常完成（安全设备的硬要求）。
 *   2) 以后要加第二个订阅者（如「变更即闪烁告警灯」）不用再动任何 CRUD 点。
 *
 * payload 可能为 NULL（见 event_bus.h 的 ev_user_changed_t 注释：
 * 「模组人脸全清」「启动对账」只想要用户页刷列表，不是一次需要上报的变更）——
 * 这里直接返回，不上报一条语义不明的空事件。 */
static void on_bus_user_changed(ev_topic_t topic, const void * payload, void * user)
{
    (void)topic;
    (void)user;
    const ev_user_changed_t * p = (const ev_user_changed_t *)payload;
    if (p == NULL) return;
    rpc_publish_event(p->evt, p->user, p->detail, p->res);
}

void app_main(void)
{
    /* 时间后端选择（规约 §5.14）：必须排在最前——TOTP、日志时间戳、
     * 状态上报、人脸事件时间戳全部依赖它。rtc 不可用时内部自动降级 sys。 */
    time_service_init();

    store_init();
    worker_init();
    hal_actuator_init();
    app_config_init();

    /* 相机链路（Sprint3 步骤 2，FR-16）：init/start 在此统一编排。
     * 无摄像头时 camera_service 自动降级 null 后端，预览区显示占位。 */
    hal_camera_init();
    hal_camera_start(320, 240);

    /* 人脸通道（步骤 3a / R6 + 3c 串口接线）：init 绑定后端（构建期默认
     * SAFE_FACE_BACKEND，环境变量运行时可覆盖）；fm225 后端的 init/start
     * 打开串口并把 fd 经 face_thread_set_uart_fd() 借给 face 线程——该接口
     * 仅线程 start 前可设（§5.19），故 face_service_init/start 必须排在
     * face_thread_start 之前（3c 调整，注释同步）。识别结果经
     * event_bus(EV_FACE_EVENT) 广播，auth_fsm 在自己的 init 里订阅。 */
    /* D2：后端名的唯一解析点在 config（环境变量 SAFE_FACE_BACKEND > 构建期默认），
     * 与下方横幅打印同一字段；此处不再二次读 env，避免"横幅 fake、实际 fm225"。 */
    face_service_init(app_config()->face_backend);
    face_service_start();

    /* 把**持久化**的人脸录入模式同步给后端（**只做一次**）。
     * 策略（users.json 的 policy.enroll_five_way）是唯一真源，env 只作初始默认 ——
     * 否则用户在界面上切到五向、重启又被 env/默认值顶回单帧。
     *
     * ⚠️ 此前这行被误放在 app_tick_fast（20ms 泵）里：它**没有幂等保护**，
     *    会以每秒 50 次的节奏反复调用 user_policy()（加锁 + 整份拷贝）与后端 setter。
     *    泵里只适合放幂等的东西（如 cred_reconcile_tick 自带 s_done 短路）。 */
    face_service_set_enroll_five_way(user_policy()->enroll_five_way);

    /* face 线程（步骤 3b，规约 §3.4）：由它 poll 相机 fd 取帧 + 转换（只转最新帧）
     * 与 FM225 UART fd（fm225 后端在上面 start 时已接入），主线程不再直接拉帧。 */
    face_thread_start();

    /* 状态机必须在界面之前初始化（UI 现代化 ui1）：
     * auth_fsm 与 ui_feedback 都订阅 EV_FACE_EVENT，总线按订阅顺序派发。
     * 先让 core（auth_fsm）消费并 bump fail_streak，UI 后读到的才是「本次失败后」
     * 的连败数；反过来会让横幅首帧显示 0/3。auth_fsm_init 不清 ui hook，安全。 */
    auth_fsm_init();
    auth_fsm_set_event_cb(on_auth_event);

    ui_init();

    event_bus_subscribe(EV_AUTH_RESULT, on_bus_auth_result, NULL);
    /* 订阅要早于 rpc_init：总线订阅与 MQTT 是否连上无关，连上后历史事件不会补发，
     * 但订阅本身必须就位，否则开机后第一次本地改用户就漏报。 */
    event_bus_subscribe(EV_USER_CHANGED, on_bus_user_changed, NULL);

    const app_config_t * cfg = app_config();
    if(cfg->mqtt_enabled) {
        rpc_init(cfg->mqtt_host, cfg->mqtt_port);
    }

    printf("[app] safe %s  platform=%s  face=%s  data=%s\n",
           SAFE_VERSION_STRING, platform_name(), cfg->face_backend, cfg->data_dir);
}

void app_tick_fast(void)
{
    worker_poll();
    event_bus_pump();
    rpc_poll();
    /* face_service_tick 保留在主线程作后端驱动（b3 取简，理由见 face_thread.c 头注释）；
     * 相机采集与格式转换已移入 face 线程，主线程不再做帧转换。
     * now_ms 必须与后端内部的 hal_time_ms() 同源（见 face_backend.h 契约）：此前传
     * 进程内自增计数，与后端的单调绝对毫秒跨基，超时判定恒真、每 tick 刷屏（D1）。 */
    face_service_tick(hal_time_ms());

    /* FR-21 防线 3「启动对账」：模组用户清单首次取得后触发一次孤儿凭据核对
     * （文件 IO 由 cred_reconcile_tick 内部经 worker_post 下沉后台线程，主线程不落盘）。
     * 放在主线程 20ms 泵里，是因为这里既是 face_service_module_users() 的合法调用点，
     * 也是 worker_post() 的合法调用点，且不引入新线程。 */
    cred_reconcile_tick();
}

void app_tick_slow(void)
{
    auth_fsm_tick();
}

void app_tick_periodic(void)
{
    rpc_publish_status_now();
}

void app_shutdown(void)
{
    /* 优雅退出链（规约 §3.2 / b4 / b5）。退出顺序：
     * 先 STREAMOFF 停流 -> 再 close(fd) -> 最后 pthread_join，避免 DQBUF 把线程卡住。 */
    face_thread_request_stop();   /* 置退出标志 + STREAMOFF 停流 + 静默在途采集 */
    hal_camera_stop();            /* 幂等：已 STREAMOFF 则 no-op */
    hal_camera_deinit();          /* munmap + close(fd) */
    face_thread_join();           /* 回收 face 线程（线程数回落） */

    face_service_stop();
    face_service_deinit();        /* §3.2：人脸服务收尾 */

    mqtt_stop();                  /* §3.2：远程通道 */

    worker_shutdown();            /* §3.2：置退出标志 + 唤醒 + pthread_join，不丢在途作业 */

    /* §3.2：store 脏数据 flush。当前 store 为同步原子落盘（R1，无内存缓存/脏标记，
     * R4/R5 未落地），凭据与日志每次落盘均走 tmp->fsync->rename 原子替换，无需额外 flush。 */

    platform_shutdown();          /* §3.2：平台收尾 */
}



