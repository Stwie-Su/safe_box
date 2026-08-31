/**
 * @file hal_face.c
 * 阶段 1 的「假 FM225」实现（fake_fm225）。
 *
 * 工作方式：
 *   - 一个无锁但有互斥保护的环形队列，存放注入的 {face_id, score}；
 *   - MQTT 线程（RPC inject_score）或 UI（滑块注入）调用 hal_face_inject 入队；
 *   - 业务层（auth_fsm 的 100ms tick）调用 hal_face_poll 非阻塞出队。
 *
 * 阶段 3 替换：把本文件换成 FM225 UART 解析（帧头 0xEF 0xAA + CRC16 + 置信度 + UserID），
 * poll 改为从串口读帧。hal_face_inject / hal_face_enroll 等接口在真实模组下语义不同，
 * 但 auth_fsm 的调用方式完全不变（NFR-5）。
 */
#include "hal_face.h"
#include <string.h>
#include <pthread.h>

#define QCAP 16

static face_result_t s_q[QCAP];
static int s_head = 0;          /* 下一个写入位置 */
static int s_tail = 0;          /* 下一个读出位置 */
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static int  s_sim_id = -1;      /* 当前模拟身份（-1 = 陌生人） */
static int  s_next_id = 1;      /* 注册时分配的递增 id */

int hal_face_init(void)
{
    s_head = s_tail = 0;
    s_sim_id = -1;
    return 0;
}

int hal_face_poll(face_result_t * out)
{
    if (!out) return -1;
    pthread_mutex_lock(&s_m);
    int empty = (s_head == s_tail);
    if (!empty) {
        *out = s_q[s_tail];
        s_tail = (s_tail + 1) % QCAP;
    }
    pthread_mutex_unlock(&s_m);
    return empty ? 0 : 1;
}

int hal_face_enroll(int * out_face_id)
{
    int id = s_next_id++;
    if (out_face_id) *out_face_id = id;
    return 0;
}

int hal_face_delete(int face_id)
{
    (void)face_id;
    return 0;
}

int hal_face_inject(int face_id, int score)
{
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    pthread_mutex_lock(&s_m);
    int next = (s_head + 1) % QCAP;
    if (next == s_tail) {                 /* 队列满：丢弃最旧，保持最新注入 */
        s_tail = (s_tail + 1) % QCAP;
    }
    s_q[s_head].face_id = face_id;
    s_q[s_head].score = score;
    s_head = next;
    pthread_mutex_unlock(&s_m);
    return 0;
}

void hal_face_set_sim_id(int face_id)
{
    s_sim_id = face_id;
}

int hal_face_sim_id(void)
{
    return s_sim_id;
}
