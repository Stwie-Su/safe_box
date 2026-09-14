/**
 * @file event_bus.c
 * 事件总线实现。
 *
 * 订阅表用固定数组，避免动态分配；队列按值拷贝 payload，
 * 这样跨线程投递时调用方不必管理事件内存的生命周期。
 */

#include "core/event_bus.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_SUB_PER_TOPIC   8
#define MAX_SUB_TOTAL       (EV_TOPIC_COUNT * MAX_SUB_PER_TOPIC)
#define QUEUE_DEPTH         32
#define PAYLOAD_MAX_BYTES   128

/* payload 结构体尺寸拘束：超过总线拷贝上限在编译期报错（新增 payload 时同步维护）。
 * _Static_assert 是 C11 关键字，本工程以 C99 + -pedantic 编译会告警（D11），
 * 故按语言标准选实现：C11 用关键字；C99 用「条件为真则长度 1、否则 -1」的数组
 * 声明——同样在编译期生效，且零告警。 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SAFE_STATIC_ASSERT(cond, msg)  _Static_assert(cond, msg)
#else
/* 双层拼接：直接写 a##__LINE__ 不会展开 __LINE__（宏参数在 ## 两侧不展开），
 * 两次展开后才是 safe_static_assert_<行号>，避免同名 typedef 重定义。 */
#define SAFE_ASSERT_CAT2(a, b)  a##b
#define SAFE_ASSERT_CAT(a, b)   SAFE_ASSERT_CAT2(a, b)
#define SAFE_STATIC_ASSERT(cond, msg) \
    typedef char SAFE_ASSERT_CAT(safe_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

SAFE_STATIC_ASSERT(sizeof(ev_auth_result_t) <= PAYLOAD_MAX_BYTES, "ev_auth_result_t exceeds bus payload cap");
SAFE_STATIC_ASSERT(sizeof(ev_face_event_t)  <= PAYLOAD_MAX_BYTES, "ev_face_event_t exceeds bus payload cap");

typedef struct {
    ev_handler_t handler;
    void *       user;
    bool         used;
} subscription_t;

typedef struct {
    ev_topic_t topic;
    size_t     size;
    uint8_t    data[PAYLOAD_MAX_BYTES];
} queued_event_t;

static subscription_t s_subs[MAX_SUB_TOTAL];

static queued_event_t s_queue[QUEUE_DEPTH];
static unsigned       s_q_head;
static unsigned       s_q_tail;
static pthread_mutex_t s_q_mutex = PTHREAD_MUTEX_INITIALIZER;

static subscription_t * slot_for(ev_topic_t topic, int * out_idx)
{
    int base = (int)topic * MAX_SUB_PER_TOPIC;
    for(int i = 0; i < MAX_SUB_PER_TOPIC; i++) {
        if(!s_subs[base + i].used) {
            *out_idx = base + i;
            return &s_subs[base + i];
        }
    }
    return NULL;
}

safe_err_t event_bus_subscribe(ev_topic_t topic, ev_handler_t handler, void * user)
{
    if(handler == NULL || topic < 0 || topic >= EV_TOPIC_COUNT) return SAFE_ERR_PARAM;

    int base = (int)topic * MAX_SUB_PER_TOPIC;
    for(int i = 0; i < MAX_SUB_PER_TOPIC; i++) {
        subscription_t * s = &s_subs[base + i];
        if(s->used && s->handler == handler && s->user == user) return SAFE_OK;   /* 重复订阅直接忽略 */
    }

    int idx = 0;
    subscription_t * s = slot_for(topic, &idx);
    if(s == NULL) return SAFE_ERR_NOMEM;

    s->handler = handler;
    s->user    = user;
    s->used    = true;
    return SAFE_OK;
}

void event_bus_unsubscribe(ev_topic_t topic, ev_handler_t handler, void * user)
{
    if(handler == NULL || topic < 0 || topic >= EV_TOPIC_COUNT) return;

    int base = (int)topic * MAX_SUB_PER_TOPIC;
    for(int i = 0; i < MAX_SUB_PER_TOPIC; i++) {
        subscription_t * s = &s_subs[base + i];
        if(s->used && s->handler == handler && s->user == user) {
            s->used    = false;
            s->handler = NULL;
            s->user    = NULL;
            return;
        }
    }
}

void event_bus_publish(ev_topic_t topic, const void * payload)
{
    if(topic < 0 || topic >= EV_TOPIC_COUNT) return;

    int base = (int)topic * MAX_SUB_PER_TOPIC;
    for(int i = 0; i < MAX_SUB_PER_TOPIC; i++) {
        subscription_t * s = &s_subs[base + i];
        /* 先复制再调用：回调里可能退订自己 */
        if(s->used) {
            ev_handler_t h = s->handler;
            void *       u = s->user;
            if(h) h(topic, payload, u);
        }
    }
}

safe_err_t event_bus_post(ev_topic_t topic, const void * payload, size_t payload_size)
{
    if(topic < 0 || topic >= EV_TOPIC_COUNT) return SAFE_ERR_PARAM;
    if(payload_size > PAYLOAD_MAX_BYTES) return SAFE_ERR_PARAM;
    if(payload_size > 0 && payload == NULL) return SAFE_ERR_PARAM;

    pthread_mutex_lock(&s_q_mutex);

    unsigned next = (s_q_head + 1u) % QUEUE_DEPTH;
    if(next == s_q_tail) {                       /* 队列满：丢弃最旧一条，保证新事件不被饿死 */
        s_q_tail = (s_q_tail + 1u) % QUEUE_DEPTH;
    }

    queued_event_t * slot = &s_queue[s_q_head];
    slot->topic = topic;
    slot->size  = payload_size;
    if(payload_size > 0) memcpy(slot->data, payload, payload_size);
    s_q_head = next;

    pthread_mutex_unlock(&s_q_mutex);
    return SAFE_OK;
}

void event_bus_pump(void)
{
    for(;;) {
        pthread_mutex_lock(&s_q_mutex);
        if(s_q_tail == s_q_head) {
            pthread_mutex_unlock(&s_q_mutex);
            return;
        }
        queued_event_t ev = s_queue[s_q_tail];
        s_q_tail = (s_q_tail + 1u) % QUEUE_DEPTH;
        pthread_mutex_unlock(&s_q_mutex);

        event_bus_publish(ev.topic, ev.size ? ev.data : NULL);
    }
}

void event_bus_reset(void)
{
    memset(s_subs, 0, sizeof(s_subs));
    pthread_mutex_lock(&s_q_mutex);
    s_q_head = 0;
    s_q_tail = 0;
    pthread_mutex_unlock(&s_q_mutex);
}
