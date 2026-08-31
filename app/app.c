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

#include "core/config.h"
#include "core/auth/auth_fsm.h"
#include "core/event_bus.h"
#include "core/remote/mqtt_client.h"
#include "core/remote/rpc.h"
#include "core/store/store.h"
#include "core/support/worker.h"
#include "hal/hal_actuator.h"
#include "hal/hal_face.h"
#include "hal/hal_time.h"
#include "platform/platform.h"
#include "ui/ui.h"
#include "app_version.h"

static uint32_t s_tick_ms;

/* 认证结果同时进总线与远程通道：
 * 走总线是为了让 UI 顶栏、告警等后续订阅方不用再改状态机；
 * 走远程通道是因为 MQTT 上报的契约来自需求文档，独立实现更清楚。 */
static void on_auth_event(const char * evt, const char * user, const char * detail, int res)
{
    ev_auth_result_t p;
    p.evt    = evt    ? evt    : "";
    p.user   = user   ? user   : "";
    p.detail = detail ? detail : "";
    p.res    = res;
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
    face_service_deinit();
    mqtt_stop();
}
