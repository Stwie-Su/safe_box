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
#include "platform/platform.h"
#include "ui/ui.h"
#include "app_version.h"

static uint32_t s_tick_ms;

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

void app_main(void)
{
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
    const char * face_backend = getenv("SAFE_FACE_BACKEND");
    if(face_backend == NULL || *face_backend == '\0') face_backend = NULL;   /* NULL = 构建期默认 */
    face_service_init(face_backend);
    face_service_start();

    /* face 线程（步骤 3b，规约 §3.4）：由它 poll 相机 fd 取帧 + 转换（只转最新帧）
     * 与 FM225 UART fd（fm225 后端在上面 start 时已接入），主线程不再直接拉帧。 */
    face_thread_start();

    ui_init();

    event_bus_subscribe(EV_AUTH_RESULT, on_bus_auth_result, NULL);

    auth_fsm_init();
    auth_fsm_set_event_cb(on_auth_event);

    const app_config_t * cfg = app_config();
    if(cfg->mqtt_enabled) {
        rpc_init(cfg->mqtt_host, cfg->mqtt_port);
    }

    printf("[app] safe %s  platform=%s  face=%s  data=%s\n",
           SAFE_VERSION_STRING, platform_name(), cfg->face_backend, cfg->data_dir);
}

void app_tick_fast(void)
{
    s_tick_ms += 20;
    worker_poll();
    event_bus_pump();
    rpc_poll();
    /* face_service_tick 保留在主线程作后端驱动（b3 取简，理由见 face_thread.c 头注释）；
     * 相机采集与格式转换已移入 face 线程，主线程不再做帧转换。 */
    face_service_tick(s_tick_ms);
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


