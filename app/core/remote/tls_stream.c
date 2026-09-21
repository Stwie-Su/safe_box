/**
 * @file tls_stream.c
 * R9 T04：传输层实现 —— **句柄式**（2026-09-21 重构）。
 * 本次落地**后端 A（恒等，明文 TCP 非阻塞）**；后端 B（mbedTLS）见 #if SAFE_FEATURE_TLS。
 *
 * ★ 这是 R9 真正的业务价值：板子上**没有可用的 ARM 版第三方 MQTT 库**
 *   （交叉编译找不到 ⇒ 编 mbed_stub ⇒ 板上远程通道永久断线）。自研不是造轮子，
 *   是因为根本没有可用的轮子。
 */
#include "core/remote/tls_stream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifdef MSG_NOSIGNAL
#define SAFE_SEND_FLAGS MSG_NOSIGNAL
#else
#define SAFE_SEND_FLAGS 0
#endif

struct tls_stream {
    int  fd;
    int  connecting;      /* 非阻塞 connect 仍在进行 */
    int  handshaking;     /* TLS 握手仍在进行（后端 B） */
    bool want_write;      /* 当前想写（供外层决定 poll 掩码） */
#if defined(SAFE_FEATURE_TLS)
    /* 后端 B（T06）：mbedtls_ssl_context / config / x509 等放这里。
     * 未实现时不允许开启该宏 —— 见下方 #error。 */
#endif
};

#if defined(SAFE_FEATURE_TLS)
#error "SAFE_FEATURE_TLS 需要后端 B（mbedTLS），T06 未实现；开启前请先实现，勿静默退化为明文。"
#endif

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    /* 关 Nagle：MQTT 是小报文为主（PINGREQ 只有 2 字节），攒包只会徒增时延。 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));
}

tls_stream_t * tls_stream_new(const tls_conf_t *cfg)
{
    (void)cfg;   /* 后端 A：忽略 TLS 配置 */
    tls_stream_t *s = (tls_stream_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    s->fd = -1;
    return s;
}

void tls_stream_free(tls_stream_t *s)
{
    if (s == NULL) return;
    tls_stream_close(s);
    free(s);
}

int tls_stream_fd(const tls_stream_t *s)
{
    return (s == NULL) ? -1 : s->fd;
}

bool tls_stream_want_write(const tls_stream_t *s)
{
    /* 明文后端：只有「非阻塞 connect 未完成」时才需要等 POLLOUT。
     * 后端 B（T06）：握手期 mbedTLS 返回 WANT_WRITE 时这里应返回 true。 */
    return (s != NULL) && (s->connecting != 0 || s->want_write);
}

void tls_stream_close(tls_stream_t *s)
{
    if (s == NULL) return;
    if (s->fd >= 0) {
        (void)shutdown(s->fd, SHUT_RDWR);
        (void)close(s->fd);
    }
    s->fd         = -1;
    s->connecting = 0;
    s->handshaking = 0;
    s->want_write  = false;
}

int tls_stream_connect(tls_stream_t *s, const char *host, int port)
{
    if (s == NULL) return -1;
    if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) return -1;

    tls_stream_close(s);

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    /* getaddrinfo 可能阻塞（DNS）。本函数在**独立的 net 线程**里调用，
     * 阻塞只影响重连节奏，不会卡住主线程 —— 也是「第 4 条线程」的必要性论据之一。 */
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { fd = -1; continue; }
        set_nonblock(fd);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            s->connecting = 0;                 /* 本地/回环常见：立即连上 */
            break;
        }
        if (errno == EINPROGRESS || errno == EALREADY) {
            s->connecting = 1;                 /* 走非阻塞路径，等 POLLOUT 再确认 */
            break;
        }
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) return -1;
    s->fd = fd;
    return fd;
}

int tls_stream_connect_poll(tls_stream_t *s, int timeout_ms)
{
    if (s == NULL || s->fd < 0) return -1;
    if (!s->connecting && !s->handshaking) return 0;

    struct pollfd pfd;
    pfd.fd      = s->fd;
    pfd.events  = POLLOUT;
    pfd.revents = 0;
    int n = poll(&pfd, 1, timeout_ms);
    if (n == 0) return 1;                    /* 超时但仍在进行 */
    if (n < 0) return (errno == EINTR) ? 1 : -1;

    /* POLLOUT 就绪**不等于**连上了：必须再查 SO_ERROR ——
     * Linux 下 connect 失败会以 POLLOUT|POLLERR 就绪，只看 POLLOUT 会把
     * 「连接被拒」误判成成功，然后第一条 CONNECT 写进去就失败，白白多一轮退避。 */
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    if (err != 0) { s->connecting = 0; return -1; }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { s->connecting = 0; return -1; }

    s->connecting = 0;
    return 0;
}

int tls_stream_write(tls_stream_t *s, const uint8_t *buf, size_t len)
{
    if (s == NULL || s->fd < 0 || buf == NULL || len == 0) return -1;
    ssize_t n = send(s->fd, buf, len, SAFE_SEND_FLAGS);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;   /* 暂不可写 */
    return -1;
}

int tls_stream_read(tls_stream_t *s, uint8_t *buf, size_t cap, int timeout_ms)
{
    if (s == NULL || s->fd < 0 || buf == NULL || cap == 0) return -2;

    if (timeout_ms > 0) {
        /* 仅为兼容「想要阻塞读」的调用方。net 线程恒传 0 —— 等待由外层 poll 统一做，
         * 避免内外两层超时互相打架（见头文件注释）。 */
        struct pollfd pfd;
        pfd.fd = s->fd; pfd.events = POLLIN; pfd.revents = 0;
        int n = poll(&pfd, 1, timeout_ms);
        if (n == 0) return -1;
        if (n < 0) return (errno == EINTR) ? -1 : -2;
    }

    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n > 0) return (int)n;
    if (n == 0) return 0;                    /* EOF：对端关闭 */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;  /* 暂无数据 */
    return -2;
}
