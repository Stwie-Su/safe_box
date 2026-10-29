/**
 * @file hal_face.h
 * 人脸模块的统一接口：业务层只看这个文件，不关心底下是哪种硬件。
 *
 * 底下可以换：假后端（测试用）、FM225 真模组、空后端（没有硬件）。
 * 换后端只改构建选项，调用方的代码一行都不用动。
 *
 * 结果不是「分数」，而是「原因」：匹配成功 / 没认出来 / 活体不过 / 超时 / 出错。
 * FM225 模组本来就不输出相似度，所以只按原因分流。
 *
 * 工作方式：调用后立刻返回，结果通过事件总线（EV_FACE_EVENT）广播出来，
 * 界面和状态机各自订阅。不要在这里轮询硬件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 识别结果的原因。 */
typedef enum {
    FACE_RES_OK = 0,        /* 认出来了 */
    FACE_RES_NO_MATCH,      /* 没认出来：计入失败次数，攒够转输动态码 */
    FACE_RES_LIVENESS_FAIL, /* 活体检测不过（可能是照片）：拒绝并告警 */
    FACE_RES_TIMEOUT,       /* 超时 / 画面里没人：不计数 */
    FACE_RES_ERROR,         /* 模组或线路异常：不计数 */
} face_reason_t;

typedef struct {
    int32_t       face_id;  /* 模组里的模板号；-1 = 没认出已注册的人 */
    face_reason_t reason;   /* 结果原因 */
    uint32_t      seq;      /* 第几次结果，去重和排查用 */
    uint32_t      timestamp;/* 结果产生的时间（Unix 秒） */
    /* 模组报回来的用户名，用来核对「这张脸是不是它说的那个人」。
     * 空串表示模组没给名字（老模板），这时跳过核对，不能因此判凭据失效。 */
    char          user_name[32];
} face_result_t;

/* 原因码转文字，打日志用。 */
const char * face_reason_name(face_reason_t r);

typedef struct {
    int32_t    face_id;    /* 成功 >= 0，失败 -1 */
    safe_err_t err;
    /* 五向录入的进度：低 5 位分别代表 上/下/左/右/正，1 表示这个方向已经录了。
     * 0x1F = 五个方向都完成。单帧模式或还没开始时是 0。 */
    uint8_t    face_dir_mask;
} face_enroll_result_t;

typedef struct {
    int32_t    face_id;
    safe_err_t err;
} face_delete_result_t;

/* 人脸事件类型。具体内容放在 event_bus 的 ev_face_event_t 里。 */
typedef enum {
    FACE_EV_DETECT = 0,   /* 识别结果 */
    FACE_EV_ENROLL_DONE,  /* 录入完成 */
    FACE_EV_DELETE_DONE,  /* 删除完成 */
    FACE_EV_ERROR,        /* 后端出错 */
} face_event_t;

/* 后端能力位。 */
#define FACE_CAP_DETECT    (1u << 0)   /* 能识别 */
#define FACE_CAP_ENROLL    (1u << 1)   /* 能录入 */
#define FACE_CAP_DELETE    (1u << 2)   /* 能删除 */
#define FACE_CAP_INJECT    (1u << 3)   /* 能注入假结果（只有假后端） */
#define FACE_CAP_LIVENESS  (1u << 4)   /* 有活体检测 */

typedef struct {
    const char * name;             /* "fake" / "fm225" / "none" */
    uint32_t     caps;
    int32_t      max_templates;    /* 最多能存多少张脸，-1 = 不限 */
} face_caps_t;

/* 初始化并选后端。backend_name 传 NULL 就用构建时的默认后端。开机调一次。 */
safe_err_t hal_face_init(const char * backend_name);
safe_err_t hal_face_deinit(void);

/* 主线程每 20ms 调一次，让后端有机会轮询硬件。
 * now_ms 必须来自 hal_time_ms() —— 两边用不同的时钟会让超时判断永远成立。 */
void hal_face_tick(uint32_t now_ms);

/* 查询能力 / 启动 / 停止 / 是否在跑。 */
const face_caps_t * hal_face_caps(void);
safe_err_t hal_face_start(void);
safe_err_t hal_face_stop(void);
bool       hal_face_running(void);

/* 人脸页是不是当前显示的页面。不在前台就停采集、停识别，省电也降温。
 * 由页面在切进来 / 切出去时调用（只有两个值，跨线程读也安全）。 */
void hal_face_set_foreground(bool active);
bool hal_face_foreground(void);

/* 开始录入（异步）。user_name 是本地的用户名，会写进模组模板里，
 * 以后识别时模组能把它带回来做核对；传 NULL 表示不写。 */
safe_err_t hal_face_enroll_async(const char * user_name);

/* 删除一张脸（异步）。 */
safe_err_t hal_face_delete_async(int32_t face_id);

/* 清空模组里所有人脸（异步）。完成事件的 face_id 固定是 -1，
 * 调用方靠这个区分「全清完了」和「删了一张」。 */
safe_err_t hal_face_delete_all_async(void);

/* 触发一次识别（不是循环识别）。结果或超时走正常事件；忙返回 SAFE_ERR_BUSY。 */
safe_err_t hal_face_verify_once(void);

/* 模组当前看到的人脸状态：0 正常，1 没看到人，2~5 分别是太靠上/下/左/右。
 * -1 = 这个后端没有这个概念，或还没收到状态。 */
int32_t hal_face_face_state(void);

/* 人脸姿态（左右转头 / 抬头低头 / 歪头）和人脸框，五向录入引导用。
 * 返回 0 = 取到；< 0 = 后端不支持。参数都可以传 NULL。 */
int32_t hal_face_face_pose(int16_t * yaw, int16_t * pitch, int16_t * roll);
int32_t hal_face_face_box(int16_t * l, int16_t * t, int16_t * r, int16_t * b);

/* 录入模式：true = 要录五个方向，false = 只录一张正脸。 */
bool hal_face_enroll_five_way(void);

/* 五向录入进度：已完成哪些方向 / 下一个该录哪个（0 = 录完了）。 */
uint8_t hal_face_enroll_dir_mask(void);
uint8_t hal_face_enroll_next_dir(void);

/* 切换录入模式。开机时按保存的设置调一次；后端不支持就忽略。 */
void hal_face_set_enroll_five_way(bool five_way);

/* 现在是不是正在录入。 */
bool hal_face_enrolling(void);

/* 模组里已经注册的人脸清单（开机对账用）。
 * 返回 >= 0 = 清单条数（0 也是有效结果，和「没取到」不一样）；
 *      -1 = 这个后端没有清单概念；
 *      -2 = 有这个能力但还没取到。
 * cap <= 0 时只问条数，ids 可以传 NULL。 */
int32_t hal_face_module_users(int32_t * ids, int32_t cap);

/* 模组健康状况。 */
typedef enum {
    FACE_MOD_UNKNOWN = 0,   /* 不知道（后端没有这个监测，或还没判断出来） */
    FACE_MOD_OK      = 1,   /* 正常：最近收到过模组的应答 */
    FACE_MOD_FAIL    = 2,   /* 不正常：连续十几秒没有任何应答 */
} face_module_health_t;

/* 查询健康状况（只读）。不健康时界面应该提示，并退回用 PIN / 动态码开锁。 */
face_module_health_t hal_face_module_health(void);

/* 健康状态转文字："unknown" / "ok" / "fail"。 */
const char * face_module_health_name(face_module_health_t h);

/* 最近一次识别结果，还没有结果时返回 NULL。 */
const face_result_t * hal_face_last_result(void);

/* 注入一条假结果，只有假后端支持（调试和远程测试用）。 */
safe_err_t hal_face_inject(int32_t face_id, face_reason_t reason);

/* 老接口：同步取最近一次结果。保留给还没改成事件驱动的调用方，以后会删。 */
int hal_face_poll_compat(face_result_t * out);

#ifdef __cplusplus
}
#endif
