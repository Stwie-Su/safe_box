/**
 * @file event_bus.h
 * 事件总线：替代原先散落在各模块的回调字段。
 *
 * 背景：状态机、RPC、主题系统各自维护一套回调指针，加一个订阅方就要改一次头文件，
 * main.c 里还要手写桥接。改为单一总线后，发布方与订阅方互不知情。
 *
 * 线程约定：
 *  - 主线程可直接 event_bus_publish()，回调同步执行，里面能安全操作 LVGL 控件；
 *  - 其它线程必须 event_bus_post()，事件进队列，由主线程的 event_bus_pump() 派发；
 *  - 总线不接管 payload 内存，发布方保证其生命周期覆盖本次派发。
 */
#pragma once

#include <stddef.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_AUTH_STATE = 0,   /* 认证状态变化：payload = const fsm_state_t * */
    EV_AUTH_RESULT,      /* 开锁结果：payload = const auth_event_t * */
    EV_USER_CHANGED,     /* 用户增删改 */
    EV_POLICY_CHANGED,   /* 安全策略或阈值变化 */
    EV_NET_CHANGED,      /* 网络配置变化 */
    EV_MQTT_STATE,       /* MQTT 连接状态：payload = const bool * */
    EV_THEME_CHANGED,    /* 主题切换：payload = const int *（主题号） */
    EV_FACE_EVENT,       /* 人脸模块事件：payload = const face_event_payload_t * */
    EV_ALARM,            /* 告警（FR-10 预留）：payload = const char * */
    EV_TOPIC_COUNT
} ev_topic_t;

/* 事件载荷：按值传递，发布方保证生命周期覆盖本次派发 */
typedef struct {
    const char * evt;
    const char * user;
    const char * detail;
    int          res;      /* 1 = 成功，0 = 失败 */
} ev_auth_result_t;

typedef void (*ev_handler_t)(ev_topic_t topic, const void * payload, void * user);

/* 订阅：同一 (topic, handler, user) 组合只登记一次。返回 SAFE_OK 或 SAFE_ERR_NOMEM / PARAM。 */
safe_err_t event_bus_subscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 退订：handler 与 user 都匹配才移除。 */
void event_bus_unsubscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 主线程发布：同步调用所有订阅者。 */
void event_bus_publish(ev_topic_t topic, const void * payload);

/* 跨线程投递：入队，等主线程 event_bus_pump() 时发布。payload 需为可复制的小结构体指针，
 * 由调用方负责其生命周期（通常指向静态或堆内存，派发完成后自行释放）。 */
safe_err_t event_bus_post(ev_topic_t topic, const void * payload, size_t payload_size);

/* 主线程周期调用：把队列里的事件派发出去。 */
void event_bus_pump(void);

/* 清空订阅与队列（测试用）。 */
void event_bus_reset(void);

#ifdef __cplusplus
}
#endif
