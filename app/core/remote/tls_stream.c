/**
 * @file tls_stream.c
 * R9 T04/T06：传输层实现 —— **句柄式**。
 *
 * 后端 A（已落地）：恒等，明文 TCP 非阻塞。局域网 + broker ACL 场景够用。
 * 后端 B（T06）  ：mbedTLS，由 SAFE_FEATURE_TLS 开启。
 *
 * ★ 这是 R9 真正的业务价值：板子上**没有可用的 ARM 版第三方 MQTT 库**
 *   （交叉编译找不到 ⇒ 编 mqtt_stub.c ⇒ 板上远程通道永久断线）。自研不是造轮子，
 *   是因为根本没有可用的轮子。
 *
 * ★★ 后端 B 的核心难点（也是本项目最能讲的一段）：
 *   TLS 握手是**多轮往返**的，而且过程中可能需要「发」也可能需要「收」。
 *   mbedTLS 用 WANT_READ / WANT_WRITE 表达「我现在想干什么」——**它自己不 poll**。
 *   所以必须由**外层**根据这两个返回值动态调整 poll 的事件掩码：
 *      WANT_READ  → 外层 poll(POLLIN)
 *      WANT_WRITE → 外层 poll(POLLOUT)
 *   若外层写死 POLLIN，握手会在需要发数据的那一轮永久卡住（既不超时也不前进）。
 *   本文件通过 tls_stream_want_write() 把这个意图暴露给外层。
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

#if defined(SAFE_FEATURE_TLS)
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#endif

#ifdef MSG_NOSIGNAL
#define SAFE_SEND_FLAGS MSG_NOSIGNAL
#else
#define SAFE_SEND_FLAGS 0
#endif

struct tls_stream {
    int  fd;
    int  connecting;      /* 非阻塞 connect 仍在进行 */
    int  handshaking;     /* TLS 握手仍在进行 */
    bool want_write;      /* 当前想写（供外层决定 poll 掩码） */
    bool tls;             /* 本句柄是否启用 TLS（后端 B） */
#if defined(SAFE_FEATURE_TLS)
    bool                     ctx_ready;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cacert;
    mbedtls_x509_crt         clicert;
    mbedtls_pk_context       pkey;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
#endif
};

/* ───────────────────────── 小工具 ───────────────────────── */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    /* 关 Nagle：MQTT 是小报文为主（PINGREQ 只有 2 字节），攒包只会徒增时延。 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));
}

#if defined(SAFE_FEATURE_TLS)
static void tls_ctx_free(tls_stream_t *s)
{
    if (!s->ctx_ready) return;
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->cacert);
    mbedtls_x509_crt_free(&s->clicert);
    mbedtls_pk_free(&s->pkey);
    mbedtls_entropy_free(&s->entropy);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    s->ctx_ready = false;
}

/* 返回 0 成功；<0 失败（调用方应按「连接失败」处理）。 */
static int tls_ctx_init(tls_stream_t *s, const tls_conf_t *cfg)
{
    memset(&s->ssl, 0, sizeof(s->ssl));
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->cacert);
    mbedtls_x509_crt_init(&s->clicert);
    mbedtls_pk_init(&s->pkey);
    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    s->ctx_ready = true;

    /* 随机数源：所有 TLS 操作的前提，失败就必须放弃（不能降级成"弱随机"）。 */
    if (mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func, &s->entropy,
                              NULL, 0) != 0) return -1;

    if (mbedtls_ssl_config_defaults(&s->conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) return -1;

    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);
    /* ★ 校验策略：verify_peer=false 时也不能"不校验"——用 OPTIONAL 而非 NONE，
     *   至少在证书存在且无效时会失败，避免"配了 CA 却没生效"的静默风险。 */
    mbedtls_ssl_conf_authmode(&s->conf,
        cfg->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_OPTIONAL);

    if (cfg->ca_file != NULL) {
        if (mbedtls_x509_crt_parse_file(&s->cacert, cfg->ca_file) != 0) return -1;
        mbedtls_ssl_conf_ca_chain(&s->conf, &s->cacert, NULL);
    }
    if (cfg->cert_file != NULL && cfg->key_file != NULL) {
        if (mbedtls_x509_crt_parse_file(&s->clicert, cfg->cert_file) != 0) return -1;
        if (mbedtls_pk_parse_keyfile(&s->pkey, cfg->key_file, NULL) != 0) return -1;
        if (mbedtls_ssl_conf_own_cert(&s->conf, &s->clicert, &s->pkey) != 0) return -1;
    }
    if (mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) return -1;
    return 0;
}
#endif /* SAFE_FEATURE_TLS */

/* ───────────────────────── 生命周期 ───────────────────────── */

tls_stream_t * tls_stream_new(const tls_conf_t *cfg)
{
    tls_stream_t *s = (tls_stream_t *)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    s->fd = -1;
#if defined(SAFE_FEATURE_TLS)
    if (cfg != NULL && cfg->enable) {
        if (tls_ctx_init(s, cfg) != 0) { tls_ctx_free(s); free(s); return NULL; }
        s->tls = true;
    }
#else
    /* 未开启 TLS 时：若调用方却要求加密，宁可失败也不要静默走明文 ——
     * 「以为在加密其实是明文」比直接报错危险得多。 */
    if (cfg != NULL && cfg->enable) { free(s); return NULL; }
#endif
    (void)cfg;
    return s;
}

void tls_stream_free(tls_stream_t *s)
{
    if (s == NULL) return;
    tls_stream_close(s);
#if defined(SAFE_FEATURE_TLS)
    tls_ctx_free(s);
#endif
    free(s);
}

int tls_stream_fd(const tls_stream_t *s)
{
    return (s == NULL) ? -1 : s->fd;
}

bool tls_stream_want_write(const tls_stream_t *s)
{
    /* 明文后端：只有「非阻塞 connect 未完成」时才需要等 POLLOUT。
     * TLS 后端：握手期 mbedTLS 返回 WANT_WRITE 时这里为 true。 */
    return (s != NULL) && (s->want_write || (s->connecting != 0));
}

void tls_stream_close(tls_stream_t *s)
{
    if (s == NULL) return;
#if defined(SAFE_FEATURE_TLS)
    if (s->tls && s->ctx_ready && s->fd >= 0) {
        /* 通知对端关闭 TLS 会话（尽力而为，非阻塞下可能发不出去，忽略结果）。 */
        (void)mbedtls_ssl_close_notify(&s->ssl);
    }
#endif
    if (s->fd >= 0) {
        (void)shutdown(s->fd, SHUT_RDWR);
        (void)close(s->fd);
    }
    s->fd          = -1;
    s->connecting  = 0;
    s->handshaking = 0;
    s->want_write  = false;
}

/* ───────────────────────── 连接 ───────────────────────── */

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

#if defined(SAFE_FEATURE_TLS)
    if (s->tls) {
        /* 把 fd 交给 mbedTLS 做 BIO。注意：mbedtls_net_send/recv 的第一个参数是
         * **指向 fd 的指针**，且握手期间 fd 不能变（重连要重新 set_bio）。 */
        mbedtls_ssl_set_bio(&s->ssl, &s->fd, mbedtls_net_send, mbedtls_net_recv, NULL);
        s->handshaking = 1;
    }
#endif
    return fd;
}

int tls_stream_connect_poll(tls_stream_t *s, int timeout_ms)
{
    if (s == NULL || s->fd < 0) return -1;

    /* ---- 阶段 1：TCP 非阻塞 connect ---- */
    if (s->connecting) {
        struct pollfd pfd;
        pfd.fd = s->fd; pfd.events = POLLOUT; pfd.revents = 0;
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
    }

#if defined(SAFE_FEATURE_TLS)
    /* ---- 阶段 2：TLS 握手（多轮往返） ---- */
    if (s->tls && s->handshaking) {
        int r = mbedtls_ssl_handshake(&s->ssl);
        if (r == 0) {                            /* 握手完成 */
            s->handshaking = 0;
            s->want_write  = false;
            /* ★ 打印协商结果：这是「链路真的加密了」的直接证据，
             *   也是排障时判断"到底有没有走 TLS"的第一手信息。 */
            printf("[TLS] 握手完成：%s / %s\n",
                   mbedtls_ssl_get_version(&s->ssl),
                   mbedtls_ssl_get_ciphersuite(&s->ssl));
            return 0;
        }
        /* ★ 关键：把 mbedTLS 的「想读/想写」翻译成外层 poll 的掩码。
         *   这里返回 1 表示「还没完，继续 poll」，外层用 tls_stream_want_write()
         *   决定监听 POLLIN 还是 POLLOUT。 */
        if (r == MBEDTLS_ERR_SSL_WANT_READ)  { s->want_write = false; return 1; }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) { s->want_write = true;  return 1; }
        /* 其它返回值都是真错误（证书校验失败/协议不匹配/…）→ 交给上层退避重连。
         * 特别地：板子 RTC 无电时系统时间回到 1970，证书 notBefore 校验会失败 ——
         * 这也是「双向 TLS 依赖校时」的由来。 */
        s->handshaking = 0;
        {
            char ebuf[160];
            mbedtls_strerror(r, ebuf, sizeof(ebuf));
            fprintf(stderr, "[TLS] 握手失败 ret=-0x%X (%s)\n", (unsigned)(-r), ebuf);
            uint32_t vflags = mbedtls_ssl_get_verify_result(&s->ssl);
            if (vflags != 0U) {
                char vbuf[512];
                mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), " ! ", vflags);
                fprintf(stderr, "[TLS] 证书校验失败：%s\n", vbuf);
            }
        }
        return -1;
    }
#endif

    return 0;
}

/* ───────────────────────── IO ───────────────────────── */

int tls_stream_write(tls_stream_t *s, const uint8_t *buf, size_t len)
{
    if (s == NULL || s->fd < 0 || buf == NULL || len == 0) return -1;

#if defined(SAFE_FEATURE_TLS)
    if (s->tls) {
        int r = mbedtls_ssl_write(&s->ssl, buf, len);
        if (r > 0) { s->want_write = false; return r; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* 记录层正在重协商/握手中：不是错误，等外层按掩码再 poll 一次。 */
            s->want_write = (r == MBEDTLS_ERR_SSL_WANT_WRITE);
            return 0;                            /* 0 = 暂不可写，调用方保留残留 */
        }
        return -1;                               /* 真错误 */
    }
#endif
    ssize_t n = send(s->fd, buf, len, SAFE_SEND_FLAGS);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;   /* 暂不可写 */
    return -1;
}

int tls_stream_read(tls_stream_t *s, uint8_t *buf, size_t cap, int timeout_ms)
{
    if (s == NULL || s->fd < 0 || buf == NULL || cap == 0) return -2;

#if defined(SAFE_FEATURE_TLS)
    if (s->tls) {
        /* ★ 不做内部 poll：等待由外层统一做（见头文件注释），避免双重等待。 */
        (void)timeout_ms;
        int r = mbedtls_ssl_read(&s->ssl, buf, cap);
        if (r > 0) { s->want_write = false; return r; }
        if (r == 0) return 0;                    /* 对端关闭 */
        if (r == MBEDTLS_ERR_SSL_WANT_READ)  { s->want_write = false; return -1; }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) { s->want_write = true;  return -1; }
        return -2;                               /* 真错误 */
    }
#endif

    if (timeout_ms > 0) {
        /* 仅为兼容「想要阻塞读」的调用方。net 线程恒传 0。 */
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
