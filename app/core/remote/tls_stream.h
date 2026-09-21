/**
 * @file tls_stream.h
 * R9 T04/T06：传输层抽象（规约 §7.5）—— **句柄式**（2026-09-21 重构）。
 *
 * 为什么要有这一层：
 *   状态机（mqtt_fsm）是纯逻辑，不能直接碰套接字；net 线程（mqtt_client.c）
 *   也不该知道「现在是明文 TCP 还是 TLS」。所有 IO 收口到这几个函数，
 *   T06 接 mbedTLS 时只换 tls_stream.c 的实现，上层零改动。
 *
 * ★★ 为什么必须从「无句柄」改成句柄式（原实现的已知债，本期偿还）：
 *   原实现是 5 个函数操作**全局单例**套接字。明文 TCP 没问题，但 mbedTLS 的
 *   ssl_context **有握手期状态**（已发 ClientHello / 等 ServerHello / 等 Finished…），
 *   无句柄就只能塞进全局，既无法表达多路，也无法表达**重握手**。
 *   句柄化后，一个连接 = 一个 tls_stream_t，状态自然地跟着连接走。
 *
 * ★★ 两条必须钉死的语义（否则 T06 必踩坑）：
 *   1. `tls_stream_read()` 是**非阻塞**的，且由外层 `poll()` 确认可读后才调用
 *      —— net 线程恒传 `timeout_ms = 0`。若让 read 内部自己 poll，就会与外层 poll
 *      形成**双重等待**：外层超时与内层超时互相打架，keepalive 定时器会被内层吞掉。
 *   2. 握手期 TLS 可能「想写而不是想读」（mbedTLS 返回 WANT_WRITE）——
 *      事件掩码必须由**外层**通过 `tls_stream_want_write()` 动态决定，
 *      不能写死 POLLIN。这是「把 TLS 嵌进非阻塞 poll 循环」的核心。
 *
 * 后端：
 *   A（已落地）—— 恒等后端：明文 TCP，非阻塞。局域网 + broker ACL 场景够用。
 *   B（T06）—— mbedTLS。cfg->enable = true 时启用；未实现时**编译期报错**，
 *              绝不静默退化成明文（那会让「以为在加密」变成真的明文）。
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
    const char *ca_file;                /* CA 证书（后端 B 用） */
    const char *cert_file;              /* 客户端证书（后端 B 用） */
    const char *key_file;               /* 客户端私钥（后端 B 用） */
    bool        verify_peer;            /* 是否校验对端证书（后端 B 用） */
    uint32_t    handshake_timeout_ms;   /* 握手超时（后端 B 用） */
} tls_conf_t;

/* 不透明句柄：一个连接一个实例。 */
typedef struct tls_stream tls_stream_t;

/** 创建句柄。cfg 可为 NULL（等价于「恒等后端」）。失败返回 NULL。 */
tls_stream_t * tls_stream_new(const tls_conf_t *cfg);

/** 释放句柄（会先关闭）。幂等，s 为 NULL 时安全。 */
void           tls_stream_free(tls_stream_t *s);

/**
 * 发起**非阻塞**连接。
 * 返回：>= 0 的套接字描述符（连接可能仍在进行中）；< 0 = 失败。
 * ★ 返回描述符**不代表已连上** —— 必须由外层 poll(POLLOUT) 就绪后调用
 *   tls_stream_connect_poll() 确认。
 */
int tls_stream_connect(tls_stream_t *s, const char *host, int port);

/**
 * 查询非阻塞连接 / TLS 握手的进度。
 * 返回： 0 = 已完成且成功；
 *        1 = 仍在进行中（外层应继续 poll，掩码见 tls_stream_want_write）；
 *       <0 = 失败（外层应按「连接失败」处理 → 关链路 + 退避重连）。
 */
int tls_stream_connect_poll(tls_stream_t *s, int timeout_ms);

/** 尽力写入。>=0 实际写入字节数（**可能为 0** = 暂不可写，调用方保留残留并在 POLLOUT 后重试）；<0 = 真错误。 */
int tls_stream_write(tls_stream_t *s, const uint8_t *buf, size_t len);

/** 非阻塞读。>0 读到的字节数；0 = 对端关闭(EOF)；-1 = 暂无数据(EAGAIN，**不是**错误)；-2 = 真错误。 */
int tls_stream_read(tls_stream_t *s, uint8_t *buf, size_t cap, int timeout_ms);

/** 取底层套接字描述符，供外层 poll 使用。未连接返回 -1。 */
int  tls_stream_fd(const tls_stream_t *s);

/**
 * ★ 握手期「当前是想写还是想读」—— 外层据此决定 poll 事件掩码：
 *     true  → 应监听 POLLOUT（TLS 想发握手报文，或 TCP 非阻塞 connect 未完成）
 *     false → 应监听 POLLIN（等对方数据）
 *   明文后端下等价于「非阻塞 connect 是否仍在进行」。
 */
bool tls_stream_want_write(const tls_stream_t *s);

/** 关闭连接（保留句柄，可再次 connect）。幂等。 */
void tls_stream_close(tls_stream_t *s);

#ifdef __cplusplus
} /*extern "C"*/
#endif
