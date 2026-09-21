/**
 * @file tls_stream.h
 * R9 T04：传输层抽象（规约 §7.5）。
 *
 * 为什么要有这一层：
 *   状态机（mqtt_fsm）是纯逻辑，不能直接碰套接字；net 线程（mqtt_client.c）
 *   也不该知道「现在是明文 TCP 还是 TLS」。于是所有 IO 都经这 5 个函数，
 *   未来 T06 接 mbedTLS 时只换本文件的实现，上层零改动。
 *
 * 后端：
 *   A（本次落地）—— 恒等后端：明文 TCP，非阻塞。局域网 + broker ACL 场景够用。
 *   B（T06 预留）—— mbedTLS。本文件已把「握手未完成」的状态位（WANT_READ /
 *                    WANT_WRITE）的语义预留好，见 tls_stream_connect_poll。
 *
 * ★ 两条必须现在钉死的语义（否则 T06 必踩坑）：
 *   1. `tls_stream_read()` 是**非阻塞**的，且由外层 `poll()` 确认可读之后才调用
 *      —— net 线程恒传 `timeout_ms = 0`。若让 read 内部自己 poll/阻塞等待，
 *      就会与外层 poll 形成**双重等待**：外层超时与内层超时互相打架，
 *      keepalive 定时器会被内层吞掉。
 *   2. 后端 B 的 WANT_READ / WANT_WRITE 必须由**外层**决定事件掩码，
 *      tls_stream 只做「有数据就解出明文，没有就返回 WANT_*」。
 *
 * ★ 已知债（写进文档，T06 前须修订）：
 *   这 4+1 个函数是**无句柄**的（内部一个全局单例套接字）。对后端 A 没问题，
 *   但 mbedTLS 的 ssl_context 有握手期状态，无句柄只能放全局、无法表达
 *   「多路 / 重握手」。T06 前应把签名改为句柄式 `tls_stream_t *`。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 连接配置。后端 A（恒等）忽略除 enable 之外的全部字段。 */
typedef struct {
    bool        enable;                 /* false = 恒等后端（明文 TCP） */
    const char *ca_file;                /* CA 证书（后端 B 用；本期未启用） */
    const char *cert_file;              /* 客户端证书（后端 B 用） */
    const char *key_file;               /* 客户端私钥（后端 B 用） */
    bool        verify_peer;            /* 是否校验对端证书（后端 B 用） */
    uint32_t    handshake_timeout_ms;   /* 握手超时（后端 B 用） */
} tls_conf_t;

/**
 * 发起**非阻塞**连接。
 * 返回：>= 0 的套接字描述符（连接可能仍在进行中）；< 0 = 失败（解析/建套接字/立即拒绝）。
 * ★ 返回描述符**不代表已连上** —— 必须由外层 poll(POLLOUT) 就绪后调用
 *   tls_stream_connect_poll() 确认。
 * cfg 可为 NULL（等价于「恒等后端」）。
 */
int tls_stream_connect(const char *host, int port, const tls_conf_t *cfg);

/**
 * 查询非阻塞连接的进度。
 * 返回： 0 = 已完成且成功；
 *        1 = 仍在进行中（外层应继续 poll(POLLOUT)）；
 *       <0 = 失败（外层应按「连接失败」处理 → 关链路 + 退避重连）。
 * timeout_ms = 0 表示「只查一次，不等待」（net 线程的用法）。
 */
int tls_stream_connect_poll(int timeout_ms);

/**
 * 尽力写入。返回：>= 0 实际写入字节数（**可能为 0** = 暂不可写，调用方应保留
 * 残留并在 POLLOUT 后重试）；< 0 = 真错误（连接已断）。
 */
int tls_stream_write(const uint8_t *buf, size_t len);

/**
 * 非阻塞读。返回：> 0 读到的字节数；
 *                 0 = 对端关闭（EOF）；
 *                -1 = 暂无数据（EAGAIN，非阻塞语义，**不是**错误）；
 *                -2 = 真错误。
 * ★ 与外层 poll 的配合：net 线程恒传 timeout_ms = 0，只在 POLLIN 就绪后调用。
 */
int tls_stream_read(uint8_t *buf, size_t cap, int timeout_ms);

/* 关闭并释放内部资源。幂等。 */
void tls_stream_close(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
