/**
 * @file fm225_proto.h
 * FM225 模组的串口协议解析：把收到的字节流切成一条条完整的帧。
 *
 * 一帧长这样：
 *   EF AA | 消息类型(1B) | 数据长度(2B，大端) | 数据(N) | 异或校验(1B)
 *   校验值 = 除 EF AA 之外所有字节按位异或。
 *
 * 只做解析，不碰串口、不用全局变量：把字节喂给 fm225_proto_feed()，
 * 解析出完整帧就回调一次。所以「收到一半」「一次收到两条」「校验错」这些情况
 * 都能在电脑上单独测。
 *
 * 谁在调：face 线程负责 feed（收串口数据），主线程 tick 负责处理「收到一半就没下文」的超时。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 协议里的固定值 ---------------- */

#define FM225_SYNC0         0xEFu
#define FM225_SYNC1         0xAAu

/* 帧类型 */
#define FM225_MSGID_REPLY   0x00u   /* 模组应答：数据 = 命令字 + 结果 + 内容 */
#define FM225_MSGID_NOTE    0x01u   /* 模组主动通知：数据 = 通知号 + 内容 */
#define FM225_MSGID_IMAGE   0x02u   /* 模组传图（本工程不解析，收到后丢弃） */

/* 下发给模组的命令字 */
#define FM225_CMD_RESET         0x10u   /* 复位 */
#define FM225_CMD_GET_STATUS    0x11u   /* 查状态 */
#define FM225_CMD_VERIFY        0x12u   /* 开始识别 */
/* 五向录入：每录好一个方向回一次，要累计到五个都完成才算结束。 */
#define FM225_CMD_ENROLL        0x13u
/* 单脸录入：只录正脸，回一次就是最终结果。
 * 注意 0x13 和 0x1D 是两套语义，不能混用。 */
#define FM225_CMD_ENROLL_SINGLE 0x1Du
#define FM225_CMD_DELETE_USER   0x20u   /* 删一个 */
#define FM225_CMD_DELETE_ALL    0x21u   /* 全删 */
#define FM225_CMD_GETUSERINFO   0x22u   /* 按编号查一个用户的信息 */
#define FM225_CMD_GET_ALL_USERID 0x24u  /* 查已注册数量和全部编号 */
/* 中断录入并清掉录入状态。注意它和 0x10（通用复位）不是一回事。 */
#define FM225_CMD_FACE_RESET    0x23u

/* 应答里的结果码 */
#define FM225_MR_SUCCESS              0x00u
#define FM225_MR_REJECTED             0x01u
#define FM225_MR_ABORTED              0x02u
#define FM225_MR_FAILED4_CAMERA       0x04u
#define FM225_MR_FAILED4_UNKNOWN      0x05u
#define FM225_MR_FAILED4_INVALIDPARAM 0x06u
#define FM225_MR_FAILED4_NOMEMORY     0x07u
#define FM225_MR_FAILED4_UNKNOWNUSER  0x08u
#define FM225_MR_FAILED4_MAXUSER      0x09u
#define FM225_MR_FAILED4_FACEENROLLED 0x0Au
#define FM225_MR_FAILED4_LIVENESS     0x0Cu
#define FM225_MR_FAILED4_TIMEOUT      0x0Du

/* 通知类型 */
#define FM225_NID_READY       0x00u
#define FM225_NID_FACE_STATE  0x01u

/* 一帧数据最多多少字节。
 * 取 256 是因为「查全部用户编号」的应答实测有 203 字节（100 个用户 × 2 字节 + 3）；
 * 早期设 128 时这种长帧会被当坏帧丢掉，而且只在真模组上才会出现。 */
#define FM225_MAX_DATA       256u

/* 两个字节之间超过这么久还没下文，就当这半帧作废，重新找帧头。 */
#define FM225_INTER_BYTE_TIMEOUT_MS  200u

/* 解析出一帧后回调拿到的内容。
 * data 指向解析器内部缓冲，**只在这次回调里有效**，不要存起来。 */
typedef struct {
    uint8_t  msgid;          /* REPLY / NOTE / IMAGE */
    uint8_t  mid_or_nid;     /* REPLY 是被应答的命令字；NOTE 是通知号 */
    uint8_t  result;         /* REPLY 的结果码；NOTE 用不到 */
    const uint8_t * data;    /* 数据区（已去掉命令字和结果码） */
    uint16_t data_len;
} fm225_frame_t;

typedef void (*fm225_frame_cb_t)(const fm225_frame_t * frame, void * user);

typedef struct fm225_proto_ctx fm225_proto_ctx_t;

/* 初始化。cb 在 feed 里被同步调用，回调里不要再调本解析器的其它函数。 */
void fm225_proto_init(fm225_proto_ctx_t * ctx, fm225_frame_cb_t cb, void * user);

/* 喂字节。串口 read 到多少就喂多少：收到一半会自动存着等下次，
 * 一次收到两条会自动切成两次回调。 */
void fm225_proto_feed(fm225_proto_ctx_t * ctx, const uint8_t * buf, size_t n);

/* 超时处理（主线程定时调）：收到一半且超过 200ms 没新字节，就把半帧丢掉。 */
void fm225_proto_tick(fm225_proto_ctx_t * ctx, uint32_t now_ms);

/* 统计：返回收到的合法帧总数；三个出参分别是被校验丢掉的、重新找帧头的、超时作废的次数。 */
size_t fm225_proto_stats(const fm225_proto_ctx_t * ctx,
                         size_t * bad_checksum, size_t * resync, size_t * timeout);

/* 解析「查全部用户编号」的应答内容。
 * 入参 data 是已经去掉命令字和结果码的部分：data[0] 是数量，后面每 2 字节一个编号。
 * 声明数量比实际多时按实际截断，不会越界读；返回解析出的编号个数。 */
int fm225_parse_userid_list(const uint8_t * data, uint16_t data_len,
                            int32_t * ids, int32_t cap);

/* 解析器的内部状态。调用方只负责分配（face 线程放一个静态的即可），不要直接改。 */
struct fm225_proto_ctx {
    uint8_t  state;              /* 当前解析到哪一步 */
    uint8_t  msgid;
    uint16_t size;               /* 帧头里声明的数据长度 */
    uint16_t got;                 /* 已经收到多少字节 */
    uint8_t  buf[FM225_MAX_DATA];/* 数据累积缓冲 */
    fm225_frame_cb_t cb;
    void *   user;
    size_t   frames;              /* 合法帧计数 */
    size_t   bad;                 /* 校验失败计数 */
    size_t   resync;              /* 重新找帧头计数 */
    size_t   timeouts;            /* 超时作废计数 */
    uint32_t last_byte_ms;        /* 最近一次收到字节的时刻 */
    bool     fed_since_tick;      /* 上次 tick 之后有没有收到新字节 */
    bool     partial;             /* 是不是正处在半帧状态 */
};

#ifdef __cplusplus
}
#endif
