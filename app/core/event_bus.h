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
 *  - payload 一律 POD（定长结构体，无指针成员）：post 按值 memcpy 进队列，
 *    发布方栈上的变量在投递后即可回收，不存在悬空指针。
 */
#pragma once

#include <stddef.h>

#include "core/err.h"
#include "hal/hal_face.h"   /* face_result_t 等（core→hal 为允许方向，同 auth_fsm.h） */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_AUTH_STATE = 0,   /* 认证状态变化：payload = const fsm_state_t * */
    EV_AUTH_RESULT,      /* 开锁结果：payload = const ev_auth_result_t * */
    EV_USER_CHANGED,     /* 用户增删改 */
    EV_POLICY_CHANGED,   /* 安全策略或阈值变化 */
    EV_NET_CHANGED,      /* 网络配置变化 */
    EV_MQTT_STATE,       /* MQTT 连接状态：payload = const bool * */
    EV_THEME_CHANGED,    /* 主题切换：payload = const int *（主题号） */
    EV_FACE_EVENT,       /* 人脸模块事件：payload = const ev_face_event_t * */
    EV_ALARM,            /* 告警（FR-10 预留）：payload 待定为 POD 结构（禁指针成员，见 §5.8） */
    EV_TOPIC_COUNT
} ev_topic_t;

/* 事件载荷：全部 POD（定长数组，禁指针成员），跨线程 post 按值拷贝。
 * 字段尺寸见《技术路线规约》§5.8；_Static_assert 保证不超总线拷贝上限。 */

/* EV_AUTH_RESULT：认证结果（evt/user/detail 截断到定长，res 1 = 成功 0 = 失败） */
typedef struct {
    char evt[16];
    char user[32];
    char detail[64];
    int  res;
} ev_auth_result_t;

/* EV_USER_CHANGED：用户 / 凭据变更（evt/user/detail 截断到定长，res 1 = 成功 0 = 失败）。
 *
 * 全部定长 char 数组 + 一个 int，**无指针成员** —— 跨线程 post 是 memcpy 按值拷贝，
 * 有指针成员的话拷过去的就是悬空地址。
 *
 * ★ 尺寸预算 128B（实测 124B，event_bus.c 里有 _Static_assert 把这条钉死）：
 *   总线硬上限其实是 192B，这里按 128B 收着用，是因为这类载荷还要**再包一层 JSON
 *   经 MQTT 上行**（PAYLOAD_MAX=512）—— 载荷越大，JSON 越容易顶到 512B 被截断，
 *   而截断是静默的（见 mqtt_client.c 的 cmdq_push）。留 64B 余量给后续字段。
 *
 * ★ detail 取 72 而不是 64：最长的审计详情是临时用户建档
 *   「<31 字符用户名> role=temp until=1730000000 limit=999」≈ 68 字节，
 *   取 64 会把 limit 截掉 —— 而「这个临时用户当初给的是几次」正是审计要看的东西。
 *
 * ★ payload **允许为 NULL**：本主题的语义是「用户数据变了」，而历史上
 *   「模组人脸全清」「启动对账」两个发布点只想要「用户页重新拉一次列表」，
 *   并不代表一次需要上报的变更。订阅方必须容忍 NULL，不能假设一定有载荷。 */
typedef struct {
    char evt[16];
    char user[32];
    char detail[72];
    int  res;
} ev_user_changed_t;

/* EV_FACE_EVENT：人脸模块事件。ev 决定哪个子结构有效：
 *  FACE_EV_DETECT     → res；FACE_EV_ENROLL_DONE → enroll；
 *  FACE_EV_DELETE_DONE→ del；FACE_EV_ERROR       → msg。 */
typedef struct {
    face_event_t         ev;
    face_result_t        res;      /* 识别结果（FACE_EV_DETECT） */
    face_enroll_result_t enroll;   /* 录入结果（FACE_EV_ENROLL_DONE） */
    face_delete_result_t del;     /* 删除结果（FACE_EV_DELETE_DONE） */
    char                 msg[64];  /* 后端错误描述（FACE_EV_ERROR） */
} ev_face_event_t;

typedef void (*ev_handler_t)(ev_topic_t topic, const void * payload, void * user);

/* 订阅：同一 (topic, handler, user) 组合只登记一次。返回 SAFE_OK 或 SAFE_ERR_NOMEM / PARAM。 */
safe_err_t event_bus_subscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 退订：handler 与 user 都匹配才移除。 */
void event_bus_unsubscribe(ev_topic_t topic, ev_handler_t handler, void * user);

/* 主线程发布：同步调用所有订阅者。 */
void event_bus_publish(ev_topic_t topic, const void * payload);

/* 跨线程投递：入队，等主线程 event_bus_pump() 时发布。payload 必须是可按值
 * 拷贝的小 POD 结构体（sizeof 见下方断言），拷贝完成后调用方即可释放/回收。 */
safe_err_t event_bus_post(ev_topic_t topic, const void * payload, size_t payload_size);

/* 主线程周期调用：把队列里的事件派发出去。 */
void event_bus_pump(void);

/* 清空订阅与队列（测试用）。 */
void event_bus_reset(void);

#ifdef __cplusplus
}
#endif
