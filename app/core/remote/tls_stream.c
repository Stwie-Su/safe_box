/**
 * @file tls_stream.c
 * R9 T04：传输层实现 —— 本次只落地**后端 A（恒等，明文 TCP 非阻塞）**。
 *
 * 为什么自研这一层而不是直接在上层写 socket：
 *   上层（mqtt_client.c 的 net 线程）只该关心「发字节 / 收字节 / 连上了没」，
 *   不该关心 TCP 还是 TLS。把 IO 收口到这 5 个函数后，T06 接 mbedTLS 只改本文件。
 *
 * ★ 这是 R9 真正的业务价值所在：板子上**没有可用的 ARM 版第三方 MQTT 库**
 *   （交叉编译找不到 ⇒ 编 mqtt_stub.c ⇒ 板上远程通道永久断线）。自研不是为了造轮子，
 *   是因为根本没有可用的轮子。
 *
 * 后端 B（mbedTLS）以 #if SAFE_FEATURE_TLS 预留：本文件保持「有数据就解明文、
 * 没有就返回状态」的无状态风格，握手期的 WANT_READ/WANT_WRITE 交给外层 poll 决定。
 */
#include "core/remote/tls_stream.h"

#include <errno.h>
#include <stdio.h>
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

/* 全局单例（无句柄签名决定的实现约束，见头文件「已知债」）。 */
static int g_fd = -1;
static int g_connecting = 0;

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    /* 关 Nagle：MQTT 是小报文为主（PINGREQ 只有 2 字节），攒包只会徒增时延。 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));
}

void tls_stream_close(void)
{
    if (g_fd >= 0) {
        (void)shutdown(g_fd, SHUT_RDWR);
        (void)close(g_fd);
    }
    g_fd = -1;
    g_connecting = 0;
}

int tls_stream_connect(const char *host, int port, const tls_conf_t *cfg)
{
#if defined(SAFE_FEATURE_TLS)
    /* 后端 B（mbedTLS）：本次不实现。若开了这个宏而没有实现，宁可编译失败，
     * 也不要静默退化成明文 —— 那会让「以为在加密」变成真的明文。 */
#error "SAFE_FEATURE_TLS 需要后端 B（mbedTLS），本期未实现；不要开启。"
#else
    (void)cfg;   /* 恒等后端：忽略 TLS 相关配置 */
#endif
    if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) return -1;

    tls_stream_close();

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    /* getaddrinfo 可能阻塞（DNS）。本函数在**独立的 net 线程**里调用，
     * 阻塞只影响重连节奏，不会卡住主线程 —— 这也是「第 4 条线程」的必要性论据之一。 */
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { fd = -1; continue; }
        set_nonblock(fd);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            g_connecting = 0;                 /* 本地/回环常见：立即连上 */
            break;
        }
        if (errno == EINPROGRESS || errno == EALREADY) {
            g_connecting = 1;                 /* 走非阻塞路径，等 POLLOUT 再确认 */
            break;
        }
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) return -1;
    g_fd = fd;
    return fd;
}

int tls_stream_connect_poll(int timeout_ms)
{
    if (g_fd < 0) return -1;
    if (!g_connecting) return 0;

    struct pollfd pfd;
    pfd.fd      = g_fd;
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
    if (getsockopt(g_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    if (err != 0) { g_connecting = 0; return -1; }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { g_connecting = 0; return -1; }

    g_connecting = 0;
    return 0;
}

int tls_stream_write(const uint8_t *buf, size_t len)
{
    if (g_fd < 0 || buf == NULL || len == 0) return -1;
    ssize_t n = send(g_fd, buf, len, SAFE_SEND_FLAGS);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;   /* 暂不可写 */
    return -1;
}

int tls_stream_read(uint8_t *buf, size_t cap, int timeout_ms)
{
    if (g_fd < 0 || buf == NULL || cap == 0) return -2;

    if (timeout_ms > 0) {
        /* 仅为兼容「想要阻塞读」的调用方。net 线程恒传 0 —— 等待由外层 poll 统一做，
         * 避免内外两层超时互相打架（见头文件注释）。 */
        struct pollfd pfd;
        pfd.fd = g_fd; pfd.events = POLLIN; pfd.revents = 0;
        int n = poll(&pfd, 1, timeout_ms);
        if (n == 0) return -1;
        if (n < 0) return (errno == EINTR) ? -1 : -2;
    }

    ssize_t n = recv(g_fd, buf, cap, 0);
    if (n > 0) return (int)n;
    if (n == 0) return 0;                    /* EOF：对端关闭 */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;  /* 暂无数据 */
    return -2;
}
