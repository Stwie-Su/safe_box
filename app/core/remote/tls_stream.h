/**
 * @file tls_stream.h
 * 网络连接抽象：明文 TCP 和 TLS 用同一套函数，上层不用关心底下是哪种。
 *
 * 为什么要包一层：状态机只做逻辑判断，不该碰 socket；网络线程也不该到处写
 * 「现在是明文还是加密」。所有网络读写都收在这几个函数里，
 * 以后换 TLS 实现，只改 tls_stream.c 一个文件。
 *
 * 句柄式：一个连接对应一个 tls_stream_t，各连各的，状态不串。
 *
 * 两个必须遵守的约定：
 *   1) read 是非阻塞的，调用前外层已经 poll 过「可读」，所以 timeout 传 0；
 *      如果 read 内部再等一次，心跳定时器会被它吞掉。
 *   2) TLS 握手期间可能「想写而不是想读」，所以 poll 监听读还是写，
 *      要问 tls_stream_want_write()，不能写死成「等可读」。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 连接配置。明文后端只用到 enable，其余字段忽略。 */
typedef struct {
    bool        enable;                 /* false = 明文 TCP */
    const char *ca_file;                /* CA 证书 */
    const char *cert_file;              /* 客户端证书（双向认证才要） */
    const char *key_file;               /* 客户端私钥 */
    bool        verify_peer;            /* 是否校验对端证书 */
    uint32_t    handshake_timeout_ms;   /* 握手超时 */
} tls_conf_t;

/* 连接句柄（内部结构不对外暴露）。 */
typedef struct tls_stream tls_stream_t;

/* 创建句柄。cfg 传 NULL 等于明文连接。失败返回 NULL（比如开了 TLS 但没编进来）。 */
tls_stream_t * tls_stream_new(const tls_conf_t *cfg);

/* 释放句柄（会先关连接）。传 NULL 安全，重复调也安全。 */
void           tls_stream_free(tls_stream_t *s);

/* 发起非阻塞连接。返回套接字描述符（>=0），失败 <0。
 * ★ 返回描述符不代表已经连上，还要等 poll 可写后调 connect_poll 确认。 */
int tls_stream_connect(tls_stream_t *s, const char *host, int port);

/* 查询连接 / 握手进度。
 *   0 = 完成；1 = 还在进行（继续 poll，方向看 want_write）；<0 = 失败。 */
int tls_stream_connect_poll(tls_stream_t *s, int timeout_ms);

/* 尽量写。>=0 = 实际写了多少字节（可能是 0，表示现在写不进去，留着下次重试）；<0 = 真错误。 */
int tls_stream_write(tls_stream_t *s, const uint8_t *buf, size_t len);

/* 非阻塞读。>0 = 读到的字节数；0 = 对方关了；-1 = 暂时没数据（不是错误）；-2 = 真错误。 */
int tls_stream_read(tls_stream_t *s, uint8_t *buf, size_t cap, int timeout_ms);

/* TLS 层里还有没有「已经解密、但上层还没取走」的数据。
 *
 * 为什么需要：TLS 一次只交出一个记录里的数据，剩下的留在 SSL 缓冲里，
 * 此时底层 socket 已经没有数据了，poll 不会再报可读 —— 不主动问一句，
 * 这些确认报文就会一直卡在里面，表现成「连着但收不到任何指令」。
 * 用法：poll 之前先问，如果 true 就立刻再读一轮。明文连接恒为 false。 */
bool tls_stream_pending(const tls_stream_t *s);

/* 取底层套接字描述符给 poll 用。没连接时返回 -1。 */
int  tls_stream_fd(const tls_stream_t *s);

/* 现在该监听「可写」还是「可读」：true 监听 POLLOUT（握手要发数据，或连接还没建好），
 * false 监听 POLLIN。 */
bool tls_stream_want_write(const tls_stream_t *s);

/* 关闭连接（句柄还能再用）。重复调安全。 */
void tls_stream_close(tls_stream_t *s);

#ifdef __cplusplus
} /*extern "C"*/
#endif
