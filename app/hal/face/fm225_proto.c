/**
 * @file fm225_proto.c
 * FM22x 协议解析器实现（Sprint3 步骤 3c，规约 §5.20）。
 *
 * 状态机（逐字节推进）：
 *   WAIT_SYNC0 → WAIT_SYNC1 → WAIT_MSGID → WAIT_SIZE_H → WAIT_SIZE_L
 *   → WAIT_DATA(N 字节) → WAIT_XOR → 回调出帧 → WAIT_SYNC0
 *
 * 容错策略：
 *   - 帧头错位（非 EF/AA 字节）：丢弃当前候选，从下一个字节重新找帧头（resync 计数）；
 *   - Size 超 FM225_MAX_DATA：按坏帧丢弃（识别链路不应出现长帧），重同步；
 *   - XOR 校验失败：丢弃整帧（bad_checksum 计数），缓冲里若还含 0xEF 则继续重同步；
 *   - 粘包：feed 循环处理，一帧校验通过立即回调，缓冲余下字节继续走状态机；
 *   - 半包：跨 feed 调用累积，state/buf/got 都在 ctx 里。
 *
 * XOR 范围（手册 §六(一)）：整条协议除去 SyncWord 部分，其余字节按位 XOR
 *   —— 即 MsgID、Size 高低位、Data 全部参与，不含 0xEF 0xAA。
 */

#include "fm225_proto.h"

#include <string.h>

typedef enum {
    ST_WAIT_SYNC0 = 0,
    ST_WAIT_SYNC1,
    ST_WAIT_MSGID,
    ST_WAIT_SIZE_H,
    ST_WAIT_SIZE_L,
    ST_WAIT_DATA,
    ST_WAIT_XOR,
} fm225_proto_state_t;

/* 坏校验丢帧后无需显式回扫：drop 已在逐字节循环里完成（当前字节重新走状态机）。 */
void fm225_proto_init(fm225_proto_ctx_t * ctx, fm225_frame_cb_t cb, void * user)
{
    if(ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = ST_WAIT_SYNC0;
    ctx->cb   = cb;
    ctx->user = user;
}

/* 帧完整收齐且 XOR 通过：组装帧视图回调出去。
 * REPLY 帧的 Data = mid + result + data[N]（手册 §M>>H REPLY 格式说明），
 * 这里把前两个字节升为 mid_or_nid / result，剩余作为 data。 */
static void dispatch_frame(fm225_proto_ctx_t * ctx)
{
    fm225_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msgid  = ctx->msgid;

    if(ctx->msgid == FM225_MSGID_REPLY && ctx->size >= 2) {
        f.mid_or_nid = ctx->buf[0];
        f.result     = ctx->buf[1];
        f.data       = ctx->buf + 2;
        f.data_len   = (uint16_t)(ctx->size - 2);
    }
    else {
        /* NOTE：Data = nid + data[N]；其余（含 IMAGE）整段 Data 给上层自取 */
        f.mid_or_nid = ctx->size >= 1 ? ctx->buf[0] : 0;
        f.result     = 0;
        f.data       = ctx->buf;
        f.data_len   = ctx->size;
    }

    ctx->frames++;
    if(ctx->cb) ctx->cb(&f, ctx->user);
}

void fm225_proto_feed(fm225_proto_ctx_t * ctx, const uint8_t * buf, size_t n)
{
    if(ctx == NULL || buf == NULL || n == 0) return;
    ctx->fed_since_tick = true;   /* 让 tick 知道有新字节（半帧超时判定用） */

    for(size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];

        switch((fm225_proto_state_t)ctx->state) {
        case ST_WAIT_SYNC0:
            if(b == FM225_SYNC0) ctx->state = ST_WAIT_SYNC1;
            else if(ctx->partial) { ctx->resync++; ctx->partial = false; }
            break;

        case ST_WAIT_SYNC1:
            if(b == FM225_SYNC1) ctx->state = ST_WAIT_MSGID;
            else {
                /* 0xEF 后不是 0xAA：0xEF 作废，但当前字节可能是新帧头 */
                ctx->resync++;
                ctx->state = (b == FM225_SYNC0) ? ST_WAIT_SYNC1 : ST_WAIT_SYNC0;
            }
            break;

        case ST_WAIT_MSGID:
            ctx->msgid = b;
            ctx->state  = ST_WAIT_SIZE_H;
            ctx->partial = true;
            break;

        case ST_WAIT_SIZE_H:
            ctx->size = (uint16_t)((uint16_t)b << 8);
            ctx->state = ST_WAIT_SIZE_L;
            break;

        case ST_WAIT_SIZE_L:
            ctx->size = (uint16_t)(ctx->size | b);
            if(ctx->size > FM225_MAX_DATA) {
                /* 超长帧：识别链路不会有（IMAGE 不走本层），按坏帧丢弃 */
                ctx->bad++;
                ctx->partial = false;
                ctx->state = ST_WAIT_SYNC0;
            }
            else if(ctx->size == 0) {
                ctx->got = 0;
                ctx->state = ST_WAIT_XOR;   /* 空 Data：直接等 XOR */
            }
            else {
                ctx->got = 0;
                ctx->state = ST_WAIT_DATA;
            }
            break;

        case ST_WAIT_DATA:
            ctx->buf[ctx->got++] = b;
            if(ctx->got >= ctx->size) ctx->state = ST_WAIT_XOR;
            break;

        case ST_WAIT_XOR: {
            /* XOR 范围：MsgID + Size(2B) + Data —— 除 SyncWord 外全部 */
            uint8_t x = ctx->msgid;
            x ^= (uint8_t)(ctx->size >> 8);
            x ^= (uint8_t)(ctx->size & 0xFF);
            for(uint16_t k = 0; k < ctx->size; k++) x ^= ctx->buf[k];
            if(x == b) {
                ctx->partial = false;
                dispatch_frame(ctx);
            }
            else {
                ctx->bad++;
                ctx->partial = false;
            }
            ctx->state = ST_WAIT_SYNC0;
            break;
        }
        }
    }
}

void fm225_proto_tick(fm225_proto_ctx_t * ctx, uint32_t now_ms)
{
    if(ctx == NULL) return;
    if(ctx->partial && ctx->fed_since_tick &&
       (now_ms - ctx->last_byte_ms) > FM225_INTER_BYTE_TIMEOUT_MS) {
        /* 帧内字节间超时：半帧作废，回找帧头。last_byte_ms 是最近一次 tick
         * 见到「feed 过新字节」的时刻（feed 无时钟参数，只能以 tick 采样时刻
         * 近似——超时阈值 200ms 远大于主线程 20ms tick 间隔，近似误差可忽略）。 */
        ctx->timeouts++;
        ctx->partial = false;
        ctx->got    = 0;
        ctx->size   = 0;
        ctx->state  = ST_WAIT_SYNC0;
    }
    ctx->last_byte_ms      = now_ms;   /* 更新采样时刻 */
    ctx->fed_since_tick    = false;
}

size_t fm225_proto_stats(const fm225_proto_ctx_t * ctx,
                         size_t * bad_checksum, size_t * resync, size_t * timeout)
{
    if(ctx == NULL) return 0;
    if(bad_checksum) *bad_checksum = ctx->bad;
    if(resync)        *resync      = ctx->resync;
    if(timeout)       *timeout     = ctx->timeouts;
    return ctx->frames;
}
