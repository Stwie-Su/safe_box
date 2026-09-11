/**
 * @file test_event_bus.c
 * 事件总线单元测试（步骤 3a：payload POD 化后的通道语义）。
 *
 * 覆盖：
 *  1) 跨线程 post 的 POD 按值拷贝正确——发布线程栈上的 payload 在 post 返回后
 *     即被改写/回收，主线程 pump 出来的副本字段必须完整（POD 语义，无悬空）；
 *  2) 多订阅者都收到；
 *  3) unsubscribe 后不再收到；
 *  4) 主线程 publish 同步语义（回调内即送达，无需 pump）。
 *
 * 不依赖 LVGL；线程用 pthread，主线程泵语义用 pump() 显式驱动。
 */
#include "test_util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "core/event_bus.h"

/* ---------------- 用例 1：跨线程 post 的 POD 按值拷贝 ---------------- */

typedef struct {
    ev_face_event_t snapshot;   /* 订阅者收到的 payload 副本 */
    int            got;
} ctx_pod_t;

static void on_face_copy(ev_topic_t topic, const void * payload, void * user)
{
    if(topic != EV_FACE_EVENT || payload == NULL) return;
    ctx_pod_t * c = (ctx_pod_t *)user;
    c->snapshot = *(const ev_face_event_t *)payload;
    c->got++;
}

/* 发布线程：post 完立即改写同一栈变量，模拟「post 后原值即失效」。
 * 总线若按值拷贝，主线程收到的应是 post 时刻的值，不是改写后的。 */
static void * poster_pod(void * arg)
{
    (void)arg;
    ev_face_event_t e;
    memset(&e, 0, sizeof(e));
    e.ev          = FACE_EV_DETECT;
    e.res.face_id = 7;
    e.res.reason  = FACE_RES_OK;
    e.res.seq     = 42;
    e.res.timestamp = 12345;
    CHECK(event_bus_post(EV_FACE_EVENT, &e, sizeof(e)) == SAFE_OK);

    memset(&e, 0xFF, sizeof(e));   /* 立即污染：若总线存的是指针，主线程将读到 0xFF 垃圾 */
    return NULL;
}

static void test_cross_thread_pod_copy(void)
{
    ctx_pod_t c;
    memset(&c, 0, sizeof(c));
    CHECK(event_bus_subscribe(EV_FACE_EVENT, on_face_copy, &c) == SAFE_OK);

    pthread_t t;
    CHECK(pthread_create(&t, NULL, poster_pod, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);

    event_bus_pump();   /* 主线程派发 */

    CHECK(c.got == 1);
    CHECK(c.snapshot.ev == FACE_EV_DETECT);
    CHECK(c.snapshot.res.face_id == 7);
    CHECK(c.snapshot.res.reason == FACE_RES_OK);
    CHECK(c.snapshot.res.seq == 42);
    CHECK(c.snapshot.res.timestamp == 12345);
}

/* ---------------- 用例 2：多订阅者都收到 ---------------- */

static int s_count_a;
static int s_count_b;
static int s_count_c;   /* 订阅同一 handler、不同 user，验证 user 区分 */

static void on_count_a(ev_topic_t topic, const void * payload, void * user)
{
    (void)payload;
    if(topic == EV_THEME_CHANGED) s_count_a += (int)(long)user;
}

static void on_count_b(ev_topic_t topic, const void * payload, void * user)
{
    (void)payload;
    if(topic == EV_THEME_CHANGED) s_count_b += (int)(long)user;
}

static void on_count_c(ev_topic_t topic, const void * payload, void * user)
{
    (void)payload;
    if(topic == EV_THEME_CHANGED) s_count_c += (int)(long)user;
}

static void test_multiple_subscribers(void)
{
    s_count_a = 0; s_count_b = 0; s_count_c = 0;
    int one = 1;

    CHECK(event_bus_subscribe(EV_THEME_CHANGED, on_count_a, (void *)(long)1) == SAFE_OK);
    CHECK(event_bus_subscribe(EV_THEME_CHANGED, on_count_b, (void *)(long)1) == SAFE_OK);
    CHECK(event_bus_subscribe(EV_THEME_CHANGED, on_count_c, (void *)(long)1) == SAFE_OK);
    CHECK(event_bus_subscribe(EV_THEME_CHANGED, on_count_a, (void *)(long)1) == SAFE_OK);   /* 重复订阅幂等 */

    int theme = 2;
    event_bus_publish(EV_THEME_CHANGED, &theme);
    event_bus_post(EV_THEME_CHANGED, &theme, sizeof(theme));
    event_bus_pump();

    CHECK(s_count_a == 2);
    CHECK(s_count_b == 2);
    CHECK(s_count_c == 2);
    (void)one;
}

/* ---------------- 用例 3：unsubscribe 后不再收到 ---------------- */

static void test_unsubscribe(void)
{
    /* 拆掉用例 2 挂的三个订阅中的两个（handler+user 都匹配才移除） */
    event_bus_unsubscribe(EV_THEME_CHANGED, on_count_a, (void *)(long)1);
    event_bus_unsubscribe(EV_THEME_CHANGED, on_count_b, (void *)(long)1);

    int theme = 3;
    event_bus_publish(EV_THEME_CHANGED, &theme);
    event_bus_post(EV_THEME_CHANGED, &theme, sizeof(theme));
    event_bus_pump();

    CHECK(s_count_a == 2);   /* 不再增长 */
    CHECK(s_count_b == 2);
    CHECK(s_count_c == 4);   /* 仍订阅，+2 */

    /* user 不匹配的退订不生效：on_count_c 换个 user 退订，应原样保留 */
    event_bus_unsubscribe(EV_THEME_CHANGED, on_count_c, (void *)(long)99);
    event_bus_publish(EV_THEME_CHANGED, &theme);
    CHECK(s_count_c == 5);   /* 仍订阅：仅 +1（这次没再 post） */
}

/* ---------------- 用例 4：主线程 publish 同步语义 ---------------- */

static ev_auth_result_t s_last_auth;
static void on_auth_sync(ev_topic_t topic, const void * payload, void * user)
{
    (void)user;
    if(topic == EV_AUTH_RESULT && payload) s_last_auth = *(const ev_auth_result_t *)payload;
}

static void test_publish_sync(void)
{
    memset(&s_last_auth, 0, sizeof(s_last_auth));
    CHECK(event_bus_subscribe(EV_AUTH_RESULT, on_auth_sync, NULL) == SAFE_OK);

    ev_auth_result_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.evt, "UNLOCK");
    strcpy(p.user, "alice");
    strcpy(p.detail, "via=face");
    p.res = 1;

    event_bus_publish(EV_AUTH_RESULT, &p);
    /* 不调 pump：publish 必须已同步送达 */
    CHECK(strcmp(s_last_auth.evt, "UNLOCK") == 0);
    CHECK(strcmp(s_last_auth.user, "alice") == 0);
    CHECK(strcmp(s_last_auth.detail, "via=face") == 0);
    CHECK(s_last_auth.res == 1);

    /* POD 截断契约（步骤 3a）：定长数组的截断责任在**填充方**（如 app.c on_auth_event
     * 先 memset 清零再 strncpy(sizeof-1)），总线只负责按值原样拷贝。此处复现填充方
     * 约定并验证副本：72 字节源 → 定长 64，NUL 结尾、前 63 字节完整。
     * 用 memcpy 显式表达截断意图，避开 strncpy 对字面量源的 -Wstringop-truncation。 */
    static const char long_src[72] =
        "0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789AB";
    memset(&p, 0, sizeof(p));
    memcpy(p.detail, long_src, sizeof(p.detail) - 1);
    event_bus_publish(EV_AUTH_RESULT, &p);
    CHECK(s_last_auth.detail[sizeof(s_last_auth.detail) - 1] == '\0');   /* NUL 恰在末位 */
    CHECK(s_last_auth.detail[62] == '2');   /* 源串第 63 字符完整保留 */
    CHECK(strlen(s_last_auth.detail) == 63);
}

/* ---------------- 用例 5：face_service_emit 跨线程并发（步骤 3c 前置必改） ---------------- */

/* 场景复刻：fm225 后端在 face 线程高频 emit（写 s_last/s_seq），同时主线程不断调
 * face_service_last_result（读 s_last）——3b 之前 emit 只在主线程无竞争，3c 起
 * face 线程 emit 与主线程读并发。断言：
 *  a) 读到的 seq 恰在 [1, N_EMIT] 范围内（无垃圾值）；
 *  b) seq 单调不回头（读到旧快照可以，但下一次读必须 >= 上一次——锁保证读写的
 *     串行化，指针返回的是锁内快照，不会出现撕裂的 face_id/reason 与 seq 组合；
 *  c) 全部 emit 落地后 last_result 的 face_id/reason 与 seq 一致（最后一帧完整）。
 * 注：本用例直接驱动 face_service（fm225 后端未编入时也成立），不依赖总线内容。 */

#include "hal/hal_face.h"
#include "hal/face/face_backend.h"   /* face_service_emit 声明（仅后端与测试可用） */

#define CONC_EMIT_N     2000     /* face 线程 emit 次数 */

static volatile int s_emit_done;

static void * emitter_thread(void * arg)
{
    (void)arg;
    for(int i = 0; i < CONC_EMIT_N; i++) {
        face_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_id   = i;
        r.reason    = FACE_RES_OK;
        r.timestamp = (uint32_t)i;
        face_service_emit(FACE_EV_DETECT, &r);
    }
    s_emit_done = 1;
    return NULL;
}

static void test_face_service_concurrent_last_result(void)
{
    /* 直接绑 fake 后端（face_service_init 幂等，重复 init 走既有后端） */
    CHECK(face_service_init("fake") == SAFE_OK);

    pthread_t t;
    uint32_t prev_seq = 0;
    int reads = 0;
    CHECK(pthread_create(&t, NULL, emitter_thread, NULL) == 0);

    /* 主线程持续读 last_result，直到 emit 线程结束（读不到结果也正常：竞争窗口） */
    while(!s_emit_done) {
        const face_result_t * r = face_service_last_result();
        if(r != NULL) {
            CHECK(r->seq >= 1 && r->seq <= CONC_EMIT_N);   /* a) 无垃圾值 */
            CHECK(r->seq >= prev_seq);                     /* b) 单调不回头 */
            CHECK(r->face_id == (int32_t)(r->seq - 1));    /* 撕裂检测：face_id 与 seq 必须配套 */
            CHECK(r->reason == FACE_RES_OK);
            prev_seq = r->seq;
            reads++;
        }
    }
    CHECK(pthread_join(t, NULL) == 0);

    /* c) 收尾后读到的是最后一帧的完整快照 */
    const face_result_t * fin = face_service_last_result();
    CHECK(fin != NULL);
    CHECK(fin->seq == CONC_EMIT_N);
    CHECK(fin->face_id == CONC_EMIT_N - 1);

    printf("  （并发用例：读 %d 次，emit %d 次）\n", reads, CONC_EMIT_N);
}

int main(void)
{
    event_bus_reset();

    test_cross_thread_pod_copy();
    test_multiple_subscribers();
    test_unsubscribe();
    test_publish_sync();
    test_face_service_concurrent_last_result();

    event_bus_reset();
    TEST_RESULT();
}
