/**
 * @file backend_wpa.c
 * WLAN 后端：wpa_supplicant 的 ctrl_iface（AF_UNIX socket）。
 *
 * 为什么选这条路（选型对比的结论，避免以后被"改简单点"推翻）：
 *   - popen("nmcli ...")：依赖 CLI 输出排版、每次 fork+exec 一个进程，
 *     桌面发行版有、板子 BusyBox 没有 → PC 与板子行为不一致；
 *   - libwpa_client/wpa_ctrl.h：PC 与板 rootfs 都没有这个头和库。
 *     引入它的下场和当年 Paho 一样——板子上静默降级，远程通道在板上永久不生效；
 *   - ctrl_iface 协议本身极简（纯文本请求 / 文本应答 + 可选异步事件），
 *     自研 ~200 行即可，且能把解析部分做成纯函数放进 ctest（见 core/net）。
 *
 * 协议要点：
 *   - socket(AF_UNIX, SOCK_DGRAM) connect 到 ctrl 路径（板子上通常是
 *     /var/run/wpa_supplicant/wlan0，可用 SAFE_WPA_CTRL 覆盖）；
 *   - 写命令字符串、读应答；失败应答以 "FAIL" 开头。
 *
 * ★ 每条命令独立 socket 的取舍：ctrl_iface 上 wpa_supplicant 会主动推异步事件
 *   （<3>CTRL-EVENT-SCAN-RESULTS 等）。长连接下事件与应答混在同一字节流，
 *   必须额外做请求/响应配对；每条命令新建 socket 并设读超时，语义最简单、
 *   不会串包。代价是每次 connect/close 的微秒级开销——相对扫描的秒级可以忽略。
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/err.h"
#include "hal/hal_net.h"

/* ctrl 路径：优先环境变量，便于 PC 上指向别的接口做实验，也便于板上接口名变化时
 * 不改代码（与项目 SAFE_* 环境变量的现有约定一致）。 */
#define WPA_CTRL_DEFAULT "/var/run/wpa_supplicant/wlan0"

/* 单次命令的读超时（秒）。扫描触发是异步的，不在超时范围内，
 * 这里只覆盖单条命令的往返。 */
#define WPA_CMD_TIMEOUT_S   5

/* 扫描等待总时长与轮询间隔：实测主动扫描在噪声环境下可能到 3~5 秒，
 * 低于 3 秒会频繁返回空列表让上层误判为"没有网络"。 */
#define WPA_SCAN_TOTAL_MS   6000
#define WPA_SCAN_POLL_MS    400

#define WPA_BUF_SCAN    8192
#define WPA_BUF_CMD      512

static const char * ctrl_path(void)
{
    const char * p = getenv("SAFE_WPA_CTRL");
    if(p != NULL && *p != '\0') return p;
    return WPA_CTRL_DEFAULT;
}

/* client 侧 socket 的自增序号：每条命令独立 bind 路径用。
 * 只在 worker 线程调用（hal 的时序约定），无需加锁。 */
static int s_seq = 0;

/* 下发一条命令并取回应答。成功返回 SAFE_OK（应答在 buf），否则返回负错误码。 */
static int wpa_cmd(const char * cmd, char * buf, size_t cap)
{
    if(cmd == NULL || buf == NULL || cap == 0) return SAFE_ERR_PARAM;

    buf[0] = '\0';

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if(fd < 0) return SAFE_ERR_IO;

    struct timeval tv;
    tv.tv_sec  = WPA_CMD_TIMEOUT_S;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* ★ client 必须显式 bind 具体路径——这是官方 wpa_ctrl 的同款做法。
     * 如果不 bind，内核会把 socket 绑到 abstract namespace 地址；真实
     * wpa_supplicant 能往 abstract 地址回包，但相当一部分工具/语言回不了
     * （对端地址拿不到具体路径），链路会以「命令发出去了、永远收不到应答」
     * 的方式坏死。显式 bind 让任何对端都能回复，也便于 ps 看到占用的路径。 */
    char cpath[108];
    snprintf(cpath, sizeof(cpath), "/tmp/wpa_ctrl_%d_%d", (int)getpid(), s_seq++);
    unlink(cpath);

    struct sockaddr_un ca;
    memset(&ca, 0, sizeof(ca));
    ca.sun_family = AF_UNIX;
    strncpy(ca.sun_path, cpath, sizeof(ca.sun_path) - 1);
    if(bind(fd, (struct sockaddr *)&ca, sizeof(ca)) != 0) {
        close(fd);
        return SAFE_ERR_IO;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ctrl_path());

    if(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        unlink(cpath);
        return SAFE_ERR_IO;
    }

    size_t  cl = strlen(cmd);
    ssize_t w  = send(fd, cmd, cl, 0);
    if(w < 0 || (size_t)w != cl) {
        close(fd);
        unlink(cpath);
        return SAFE_ERR_IO;
    }

    ssize_t r = recv(fd, buf, cap - 1, 0);
    if(r < 0) {
        int e = errno;
        close(fd);
        unlink(cpath);
        return (e == EAGAIN || e == EWOULDBLOCK) ? SAFE_ERR_TIMEOUT : SAFE_ERR_IO;
    }
    buf[r] = '\0';
    close(fd);
    unlink(cpath);

    /* 应答以 FAIL / UNKNOWN 开头表示命令本身被拒。注意 ADD_NETWORK 成功返回的是
     * 数字 id，使能类命令返回 OK，都不能按 OK 一刀切，所以这里只识别失败前缀。 */
    if(strncmp(buf, "FAIL", 4) == 0)    return SAFE_ERR_FAIL;
    if(strncmp(buf, "UNKNOWN", 7) == 0) return SAFE_ERR_UNSUP;
    return SAFE_OK;
}

/* 变参版：构造命令后调用 wpa_cmd。 */
static int wpa_cmdf(const char * fmt, ...)
{
    char cmd[WPA_BUF_CMD];
    char buf[WPA_BUF_CMD];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    if(n < 0 || (size_t)n >= sizeof(cmd)) return SAFE_ERR_PARAM;
    return wpa_cmd(cmd, buf, sizeof(buf));
}

/* 字符串 → 十六进制串。
 * ★ 为什么用 hex 而不是 "\"ssid\"" 这种引号包裹：SSID/PSK 里完全可能出现
 *   引号、反斜杠、空格、非 ASCII（中文 SSID 很常见）。引号形式需要转义规则，
 *   一旦实现不全就会被注入多余字段（例如密码里带引号直接改写后续配置）。
 *   hex 形式由 wpa_supplicant 无条件接受，没有任何转义歧义。 */
static int to_hex(const char * s, char * out, size_t cap)
{
    if(s == NULL || out == NULL || cap == 0) return SAFE_ERR_PARAM;
    if(cap < 3) return SAFE_ERR_PARAM;

    size_t n = 0;
    static const char HX[] = "0123456789abcdef";
    for(size_t i = 0; s[i] != '\0' && n + 2 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        out[n++] = HX[c >> 4];
        out[n++] = HX[c & 0x0f];
    }
    out[n] = '\0';
    return SAFE_OK;
}

static void nap_ms(int ms)
{
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)select(0, NULL, NULL, NULL, &tv);
}

/* ---------------- 后端实现 ---------------- */

static int wpa_available(void)
{
    char buf[WPA_BUF_CMD];
    /* PING 是从不做副作用的探测命令，回 PONG。用它判断 socket 可达且 wpa 活着。 */
    return wpa_cmd("PING", buf, sizeof(buf)) == SAFE_OK;
}

static int wpa_scan(wpa_ap_t * out, size_t cap)
{
    if(out == NULL && cap > 0) return SAFE_ERR_PARAM;
    if(cap == 0) return 0;

    char buf[WPA_BUF_SCAN];

    /* 触发扫描。返回 FAIL 多半是 FAIL-BUSY（上一轮扫描还没结束），
     * 这不是致命错误：已有扫描的结果照样能取，所以继续走轮询。 */
    int rc = wpa_cmd("SCAN", buf, sizeof(buf));
    if(rc != SAFE_OK && rc != SAFE_ERR_FAIL && rc != SAFE_ERR_UNSUP) return rc;

    /* 轮询直到拿到非空结果或超时。为什么不一次性等到 X 毫秒再读：
     * 有的环境 1 秒就出结果，固定等会让 UI 无谓地转圈。 */
    for(int waited = 0; waited < WPA_SCAN_TOTAL_MS; waited += WPA_SCAN_POLL_MS) {
        nap_ms(WPA_SCAN_POLL_MS);
        if(wpa_cmd("SCAN_RESULTS", buf, sizeof(buf)) != SAFE_OK) continue;

        int n = wpa_parse_scan_results(buf, out, cap);
        if(n > 0) return n;   /* 返回个数；>0 与 SAFE_OK(0) 不冲突：0 表示没扫到 */
    }

    return 0;   /* 超时且始终为空：可能是环境里真没有热点，不算错误 */
}

static int wpa_status(net_status_t * st)
{
    if(st == NULL) return SAFE_ERR_PARAM;

    char buf[WPA_BUF_SCAN];
    int rc = wpa_cmd("STATUS", buf, sizeof(buf));
    if(rc != SAFE_OK) return rc;

    memset(st, 0, sizeof(*st));

    char v[128];
    if(wpa_status_get(buf, "wpa_state", v, sizeof(v)) == 0) {
        if(strcmp(v, "COMPLETED") == 0) {
            st->state = NET_STATE_CONNECTED;
        }
        else if(strcmp(v, "SCANNING") == 0) {
            st->state = NET_STATE_SCANNING;
        }
        else if(strcmp(v, "ASSOCIATING") == 0 || strcmp(v, "ASSOCIATED") == 0 ||
                strcmp(v, "4WAY_HANDSHAKE") == 0 || strcmp(v, "GROUP_HANDSHAKE") == 0) {
            st->state = NET_STATE_CONNECTING;
        }
        else {
            /* DISCONNECTED / INACTIVE / INTERFACE_DISABLED / DORMANT ... */
            st->state = NET_STATE_DOWN;
        }
    }
    else {
        st->state = NET_STATE_UNKNOWN;
    }

    if(wpa_status_get(buf, "ssid", st->ssid, sizeof(st->ssid)) != 0) st->ssid[0] = '\0';

    /* ip_address 只有在外部 DHCP 客户端拿到地址后才会出现。
     * 取不到不代表没连上——WPA 握手完成但 DHCP 未完成时就是这个状态。 */
    if(wpa_status_get(buf, "ip_address", st->ip, sizeof(st->ip)) != 0) st->ip[0] = '\0';

    /* STATUS 里不含 RSSI（要拿信号得用 SIGNAL_POLL 或匹配 SCAN_RESULTS）。
     * 这里保持 0 = 未知，不编造数值。 */
    st->signal_dbm = 0;

    return SAFE_OK;
}

static int wpa_saved(net_saved_t * out, size_t cap)
{
    if(out == NULL && cap > 0) return SAFE_ERR_PARAM;
    if(cap == 0) return 0;

    char buf[WPA_BUF_SCAN];
    if(wpa_cmd("LIST_NETWORKS", buf, sizeof(buf)) != SAFE_OK) return SAFE_ERR_IO;

    return wpa_parse_list_networks(buf, out, cap);
}

static int wpa_connect(const char * ssid, const char * psk)
{
    if(ssid == NULL || *ssid == '\0') return SAFE_ERR_PARAM;

    char  buf[WPA_BUF_CMD];
    char  hex[(WPA_SSID_MAX + 1) * 2 + 1];
    char  hpw[256];
    int   rc;

    /* 1) 新建网络，拿到 id */
    rc = wpa_cmd("ADD_NETWORK", buf, sizeof(buf));
    if(rc != SAFE_OK) return rc;

    int id = atoi(buf);
    if(id < 0) return SAFE_ERR_FAIL;

    /* 失败路径统一清理：不留半配置的网络块，否则下次 LIST_NETWORKS 会看到垃圾，
     * 而且 ENABLE 一半失败会让 wpa 在不同网络间反复重试。 */
    #define FAIL_GOTO(r) do { rc = (r); if(rc != SAFE_OK) goto cleanup; } while(0)

    FAIL_GOTO(to_hex(ssid, hex, sizeof(hex)));
    FAIL_GOTO(wpa_cmdf("SET_NETWORK %d ssid %s", id, hex));

    if(psk != NULL && *psk != '\0') {
        FAIL_GOTO(to_hex(psk, hpw, sizeof(hpw)));
        FAIL_GOTO(wpa_cmdf("SET_NETWORK %d psk %s", id, hpw));
        FAIL_GOTO(wpa_cmdf("SET_NETWORK %d key_mgmt WPA-PSK", id));
    }
    else {
        FAIL_GOTO(wpa_cmdf("SET_NETWORK %d key_mgmt NONE", id));
    }

    FAIL_GOTO(wpa_cmdf("ENABLE_NETWORK %d", id));
    /* SELECT_NETWORK 让 wpa 立刻切到这个网络；否则它会按自己的策略慢慢挑。 */
    FAIL_GOTO(wpa_cmdf("SELECT_NETWORK %d", id));

    /* 落盘失败不致命（内存里的配置已生效，重启后丢失而已），所以不 goto cleanup */
    (void)wpa_cmd("SAVE_CONFIG", buf, sizeof(buf));

    #undef FAIL_GOTO
    return SAFE_OK;

cleanup:
    (void)wpa_cmdf("REMOVE_NETWORK %d", id);
    return rc;
}

static int wpa_disconnect(void)
{
    char buf[WPA_BUF_SCAN];

    (void)wpa_cmd("DISCONNECT", buf, sizeof(buf));

    /* 顺带清理已保存的网络块。"忘记网络"如果不删配置，wpa 会自己重连。 */
    if(wpa_cmd("LIST_NETWORKS", buf, sizeof(buf)) == SAFE_OK) {
        const char * p = buf;
        while(*p != '\0') {
            const char * eol = strchr(p, '\n');
            const char * end = eol ? eol : p + strlen(p);

            /* 行格式：<id>\t<ssid>\t<bssid>\t<flags>，表头是 network id / ssid / ... */
            if(end > p && memcmp(p, "network id", 10) != 0) {
                int id = atoi(p);
                /* atoi 对表头返回 0；wpa 的网络 id 从 0 开始，
                 * 所以额外要求首字符是数字，避免误删 id 0 或误解析表头。 */
                if(id >= 0 && p[0] >= '0' && p[0] <= '9') {
                    (void)wpa_cmdf("REMOVE_NETWORK %d", id);
                }
            }
            if(eol == NULL) break;
            p = eol + 1;
        }
    }

    (void)wpa_cmd("SAVE_CONFIG", buf, sizeof(buf));
    return SAFE_OK;
}

const net_backend_t net_backend_wpa = {
    .name       = "wpa",
    .available  = wpa_available,
    .scan       = wpa_scan,
    .status     = wpa_status,
    .saved      = wpa_saved,
    .connect    = wpa_connect,
    .disconnect = wpa_disconnect,
};

