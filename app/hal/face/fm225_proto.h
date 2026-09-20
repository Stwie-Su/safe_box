/**
 * @file fm225_proto.h
 * FM22x 协议解析器——纯状态机（Sprint3 步骤 3c，规约 §5.20）。
 *
 * 帧格式（《FM22x 系列人脸锁算法模组用户开发手册 V1.7》§六(一)）：
 *   SyncWord(0xEF 0xAA) + MsgID(1B) + Size(2B 大端) + Data(N, 0<=N<=65535) + XOR(1B)
 *   XOR = 除 SyncWord 外全部字节（MsgID + Size + Data）按位 XOR。
 *   手册示例验算：EF AA 00 00 05 13 00 00 03 1F 0A（Size=5 大端，XOR=0x0A ✓）
 *
 * 设计约束（c1）：
 *   - 纯状态机，不碰 fd / 串口 / 全局变量——fm225_proto_feed(ctx, buf, n) 只吃
 *     字节流吐帧事件，单测可以脱离串口逐字节驱动（半包 / 粘包 / 坏校验 / 超时）；
 *   - ctx 由调用方分配（face 线程持一个静态 ctx），解析器内部零动态分配；
 *   - 帧数据上限 FM225_MAX_DATA：识别链路只会收到 REPLY/NOTE 短帧，IMAGE 传图
 *     （可达 64KB）不走本解析器，超长帧按坏帧丢弃重同步。
 *
 * 线程模型：ctx 单线程 feed（face 线程）；超时由 tick（主线程/backend）推进——
 *   feed 无时钟参数，tick 以「两次 tick 的间隔 + 是否 feed 过新字节」近似判定半帧
 *   超时（阈值 200ms 远大于 20ms tick 间隔，近似误差可忽略）。tick 只在超时成立时
 *   复位解析状态，与 feed 的写路径通过 fed_since_tick/last_byte_ms 两个标量交接。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 协议常量（FM22x 手册 V1.7） ---------------- */

#define FM225_SYNC0         0xEFu
#define FM225_SYNC1         0xAAu

/* M>>H 消息 ID */
#define FM225_MSGID_REPLY   0x00u   /* 模组应答：Data = mid(1B) + result(1B) + data(N) */
#define FM225_MSGID_NOTE    0x01u   /* 模组主动通知：Data = nid(1B) + data(N) */
#define FM225_MSGID_IMAGE   0x02u   /* 模组传图（本期不解析，按普通帧送出后由上层丢弃） */

/* H>>M 命令字（backend_fm225 下发用） */
#define FM225_CMD_RESET         0x10u
#define FM225_CMD_GET_STATUS    0x11u
#define FM225_CMD_VERIFY        0x12u
/* 0x13 ENROLL_5：五向录入。face_dir 是位掩码；**应答是「单方向成功」的进度**，
 *   需累计到 0x1F 才算全部完成（会有多帧应答，不是一次定案）。 */
#define FM225_CMD_ENROLL        0x13u
/* 0x1D ENROLL_SINGLE：单脸录入。face_dir = 0x01（正脸）；**应答即最终结果**。
 *   依据：桌面 FM225 测试工具（fm225_debug_platform）的命令表与应答解析，
 *   两处一致 —— 单脸用 0x1D、五向用 0x13，二者应答语义不同，不可混用。 */
#define FM225_CMD_ENROLL_SINGLE 0x1Du
#define FM225_CMD_DELETE_USER   0x20u
#define FM225_CMD_DELETE_ALL    0x21u
/* 查询类命令（对账用，FR-21 防线 3）：
 *   GETUSERINFO   按 uid 取单个用户信息（id + user_name[32] + admin）
 *   GET_ALL_USERID 取「已注册数量 + 全部 ID 列表」（无 data 请求） */
#define FM225_CMD_GETUSERINFO   0x22u
#define FM225_CMD_GET_ALL_USERID 0x24u
/* 终止录入并清除录入状态（手册 §六「录入过程中可通过 FACE RESET 指令终止录入，
 * 先前的录入状态也会清零」）。注意与 FM225_CMD_RESET(0x10 = MID_RESET，通用复位)
 * 不是一回事——代码注释曾把 0x10 称作 FACE RESET，属术语混用。 */
#define FM225_CMD_FACE_RESET    0x23u

/* REPLY.Data 首字节：被应答的命令字 mid */
/* REPLY.Data 次字节：结果码 MR_* */
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
#define FM225_MR_FAILED4_TIMEOUT      0x0D

/* NOTE.Data 首字节 nid */
#define FM225_NID_READY       0x00u
#define FM225_NID_FACE_STATE  0x01u

/* 帧数据上限（v1.4：128 → 256）
 *
 * 128 装不下 GET_ALL_USERID 的应答：真机实测（2026-09-15）该帧 Size=0x00CB=203 字节
 * （mid(1) + result(1) + user_counts(1) + users_id[100×2]），沿用 128 会把整帧当坏帧丢弃、
 * 对账功能静默失效。**这是一条 PC 模拟器永远不产生、只有真模组才暴露的长帧**。
 * IMAGE 传图（可达 64KB）仍不走本层（按坏帧重同步丢弃）。
 * 代价：解析器 ctx 由 128B 涨到 256B（face 线程持一个静态 ctx，可忽略）。 */
#define FM225_MAX_DATA       256u

/* 帧内字节间超时（ms）：115200 波特率一字节约 87us，200ms 余量 2000+ 字节。
 * 超过该间隔无后续字节视为半帧作废（回 WAIT_SYNC0）。 */
#define FM225_INTER_BYTE_TIMEOUT_MS  200u

/* ---------------- 帧事件 ---------------- */

/* 解析出一帧时回调收到的帧视图。
 * data 指向解析器内部帧缓冲，**仅本次回调内有效**——回调返回后缓冲即被复用。 */
typedef struct {
    uint8_t  msgid;          /* FM225_MSGID_REPLY / NOTE / IMAGE */
    uint8_t  mid_or_nid;     /* REPLY: 被应答命令字；NOTE: 通知字 */
    uint8_t  result;         /* REPLY: MR_* 结果码；NOTE: 未用（0） */
    const uint8_t * data;    /* Data 区（去掉 REPLY 的 mid/result 前缀） */
    uint16_t data_len;       /* data 有效长度 */
} fm225_frame_t;

typedef void (*fm225_frame_cb_t)(const fm225_frame_t * frame, void * user);

/* ---------------- 状态机 ---------------- */

typedef struct fm225_proto_ctx fm225_proto_ctx_t;

/* 初始化解析器：cb 在 feed 解出完整帧时被同步调用（feed 的调用线程内）。
 * 重入说明：cb 内禁止再调 fm225_proto_* 本 ctx 的任何接口。 */
void fm225_proto_init(fm225_proto_ctx_t * ctx, fm225_frame_cb_t cb, void * user);

/* 喂入收到的字节（任意长度：串口一次 read 的结果直接灌入）。
 * 半包自动跨调用累积；粘包自动切分（一次 feed 可能触发多次回调）。 */
void fm225_proto_feed(fm225_proto_ctx_t * ctx, const uint8_t * buf, size_t n);

/* 超时推进（主线程/backend tick 驱动，now_ms 为单调毫秒）：
 * 帧收到一半且距上次喂字节超过 FM225_INTER_BYTE_TIMEOUT_MS 时作废半帧。
 * 超时仅复位解析状态（不触碰帧缓冲中间态的读路径），feed/tick 可跨线程调用。 */
void fm225_proto_tick(fm225_proto_ctx_t * ctx, uint32_t now_ms);

/* 解析统计（诊断用）：返回收到的合法帧总数，出参可传 NULL 跳过。
 * bad_checksum = XOR 校验失败丢弃的帧数；resync = 垃圾字节后重找帧头次数；
 * timeout = 半帧超时作废次数。 */
size_t fm225_proto_stats(const fm225_proto_ctx_t * ctx,
                         size_t * bad_checksum, size_t * resync, size_t * timeout);

/* ---------------- 业务载荷解析（纯函数） ----------------
 *
 * GET_ALL_USERID(0x24) 应答载荷解析。入参 data/data_len 是【已剥掉 REPLY 的
 * mid/result 前缀】的 Data 区（即 fm225_frame_t.data / data_len）：
 *   data[0]           = user_counts（声明的用户数，1B）
 *   data[1 .. 1+2n-1] = n 个「高字节在前」的 16 位 uid（每 2B 一个）
 * 本函数只认「剥前缀后的视图」——若把 mid/result 也算进来就会整体错位 2 字节
 * （backend_fm225.c 曾因此读 data[2]、把空模块的 0 当成数量而掩盖问题）。抽成
 * 纯函数以便脱离串口单测固化边界：声明量>实际、空清单、奇数尾字节、缓冲上限。
 *
 * 容错：声明量 > 实际可容纳 uid 数时按实际截断（绝不越界读）；data=NULL/data_len<1/
 * data[0]<=0 返回 0；cap 为 ids 缓冲容量（<0 视为 0）。返回解析出的 uid 个数（>= 0）。 */
int fm225_parse_userid_list(const uint8_t * data, uint16_t data_len,
                            int32_t * ids, int32_t cap);

/* 内部状态（外部只分配，不直接访问；定义放 .c 里防扩散实现细节）。
 * 尺寸对齐 4 字节方便静态/栈上分配。 */
struct fm225_proto_ctx {
    uint8_t  state;              /* fm225_proto_state_t */
    uint8_t  msgid;
    uint16_t size;               /* 声明的 Data 长度 */
    uint16_t got;                 /* 已收到的 Data 字节数 */
    uint8_t  buf[FM225_MAX_DATA];/* Data 累积缓冲 */
    fm225_frame_cb_t cb;
    void *   user;
    size_t   frames;              /* 合法帧计数 */
    size_t   bad;                 /* 坏校验计数 */
    size_t   resync;              /* 重同步计数 */
    size_t   timeouts;            /* 超时作废计数 */
    uint32_t last_byte_ms;        /* 最近一次 tick 采样时刻（超时基准，tick 维护） */
    bool     fed_since_tick;      /* 自上次 tick 后 feed 过字节（超时判定用） */
    bool     partial;             /* 是否处于帧中间态（WAIT_* 非 SYNC 阶段） */
};

#ifdef __cplusplus
}
#endif
