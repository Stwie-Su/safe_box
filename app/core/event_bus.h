/**
 * @file event_bus.h
 * 事件总线：模块之间不直接互相调用，而是「发一条消息」，谁关心谁订阅。
 *
 * 好处：加一个订阅方（比如顶栏要显示状态）不用改发布方那一行代码。
 *
 * 线程规则（很重要）：
 *   - 主线程：直接 event_bus_publish()，订阅回调当场同步执行，回调里可以改界面；
 *   - 其它线程（worker / MQTT / face）：只能 event_bus_post()，消息进队列，
 *     等主线程 event_bus_pump() 时才真正派发——所以回调永远在主线程跑。
 *
 * 消息内容一律用「定长结构体、里面没有指针」，post 时整块拷进队列，
 * 发布方函数返回后栈上的变量就没了也没关系，不会出现野指针。
 */
#pragma once

#include <stddef.h>

#include "core/err.h"
#include "hal/hal_face.h"   /* face_result_t 等类型 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_AUTH_STATE = 0,   /* 认证状态变了：payload = const fsm_state_t * */
    EV_AUTH_RESULT,      /* 开锁结果：payload = const ev_auth_result_t * */
    EV_USER_CHANGED,     /* 用户增删改 */
    EV_POLICY_CHANGED,   /* 安全策略或阈值变了 */
    EV_NET_CHANGED,      /* 网络配置变了 */
    EV_MQTT_STATE,       /* 远程连接状态：payload = const bool * */
    EV_THEME_CHANGED,    /* 换主题：payload = const int *（主题号） */
    EV_FACE_EVENT,       /* 人脸模块事件：payload = const ev_face_event_t * */
    EV_ALARM,            /* 告警（预留）：payload 以后定 */
    EV_TOPIC_COUNT
} ev_topic_t;

/* 认证结果 */
typedef struct {
    char evt[16];
    char user[32];
    char detail[64];
    int  res;            /* 1 = 成功，0 = 失败 */
} ev_auth_result_t;

/* 用户 / 凭据变更。
 *
 * 字段全是定长数组、没有指针，才能跨线程整块拷贝。
 * detail 取 72 字节：最长的那条是临时用户建档信息
 * （「<用户名> role=temp until=1730000000 limit=999」约 68 字节），
 * 取 64 会把最后的 limit 截掉，而「当时给的是几次」恰好是审计要看的。
 *
 * payload 允许为 NULL：这条消息的意思是「用户数据变了」，
 * 而「模组人脸全清」「开机对账」只希望用户页刷新一下列表，并不需要上报，
 * 所以订阅方要能接受 NULL。 */
typedef struct {
    char evt[16];
    char user[32];
    char detail[72];
    int  res;
} ev_user_changed_t;

/* 人脸事件。ev 决定哪个字段有效：
 *   FACE_EV_DETECT      → res
 *   FACE_EV_ENROLL_DONE → enroll
 *   FACE_EV_DELETE_DONE → del
 *   FACE_EV_ERROR       → msg */
typedef struct {
    face_event_t         ev;
    face_result_t        res;      /* 识别结果 */
    face_enroll_result_t enroll;   /* 录入结果 */
    face_delete_result_t del;      /* 删除结果 */
    char                 msg[64];  /* 出错描述 */
} ev_face_event_t;

typedef void (*ev_handler_t)(ev_topic_t topic, const void * payload, void * user);

/* 订阅。同一个 (topic, handler, user) 重复订阅只算一次。 */
safe_err_t event_bus_subscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 退订：handler 和 user 都对上才移除。 */
void event_bus_unsubscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 主线程发布：立刻依次调用所有订阅者。 */
void event_bus_publish(ev_topic_t topic, const void * payload);

/* 跨线程投递：先入队，等主线程 pump 时再发布。
 * payload 必须是可按值拷贝的小结构体，拷完调用方就可以回收它。 */
safe_err_t event_bus_post(ev_topic_t topic, const void * payload, size_t payload_size);

/* 主线程每 20ms 调一次：把队列里的消息派发出去。 */
void event_bus_pump(void);

/* 清空订阅和队列（只有测试会用）。 */
void event_bus_reset(void);

#ifdef __cplusplus
}
#endif
