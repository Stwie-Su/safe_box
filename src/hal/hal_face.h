/**
 * @file hal_face.h
 * 人脸识别数据源抽象（需求 FR-2 / M8-M9）。
 *
 * 铁律（NFR-5）：业务层（auth_fsm）只调本接口，不得感知数据来自模拟器还是真实 FM225。
 * 阶段 1 = fake_fm225（UI 滑块 / RPC inject_score 注入）；
 * 阶段 3 = FM225 UART 解析，只需替换 hal_face.c 实现，接口不变。
 *
 * 数据契约：poll 返回 {face_id, score}；face_id==-1 表示「未识别到任何人脸」。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  face_id;   /* -1 = 未识别 */
    int  score;     /* 0~100，越界值视为无效 */
} face_result_t;

/* 初始化（阶段 1 无需硬件；阶段 3 打开串口） */
int hal_face_init(void);

/* 非阻塞取一帧识别结果；无结果返回 0（不阻塞、不清空队列的其它项） */
int hal_face_poll(face_result_t * out);

/* 注册人脸，返回新分配的 face_id（阶段 1 为递增假 id） */
int hal_face_enroll(int * out_face_id);

/* 删除已注册人脸 */
int hal_face_delete(int face_id);

/* ★ 调试接口（仅 fake 实现有效）：注入一次识别结果（face_id 可传 -1 表示陌生人） */
int hal_face_inject(int face_id, int score);

/* 设置/读取「当前模拟身份」：UI 滑块注入时使用的默认 face_id */
void hal_face_set_sim_id(int face_id);
int  hal_face_sim_id(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
