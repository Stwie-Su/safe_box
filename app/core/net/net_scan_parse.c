/**
 * @file net_scan_parse.c
 * wpa_supplicant 应答的纯解析实现（契约见 net_scan_parse.h）。
 *
 * 实现纪律：
 *  - 全程只在调用方给的缓冲区里写，不 malloc——HAL 层可能在没有 MMU 压力之外
 *    的场景被反复调用，且单测要能验证「零分配」这条性质；
 *  - 任何来自网络的字段都当作不可信：长度先边界检查再拷贝；
 *  - 不修改入参 text（const），用 [beg,end) 双指针切分行与字段。
 */

#include "core/net/net_scan_parse.h"

#include <stddef.h>
#include <string.h>

/* 把 [b,e) 拷进 dst（最多 cap-1 字节），保证 NUL 结尾。cap==0 时直接失败返回 -1。 */
static int copy_field(const char * b, const char * e, char * dst, size_t cap)
{
    if(dst == NULL || cap == 0) return -1;
    if(b == NULL || e == NULL || e < b) {
        dst[0] = '\0';
        return -1;
    }

    size_t n = (size_t)(e - b);
    if(n > cap - 1) n = cap - 1;   /* 截断而非报错：宁可拿可读前缀，也不返回空 */
    memcpy(dst, b, n);
    dst[n] = '\0';
    return (int)n;
}

/* 定长整数解析（不要求 NUL 结尾）。支持负号（RSSI 为负值）。
 * 返回 0 成功，-1 非法（含空串、非数字字符、超量程）。 */
static int parse_int(const char * s, size_t len, int * out)
{
    if(s == NULL || len == 0 || out == NULL) return -1;

    size_t i = 0;
    int sign = 1;
    if(s[0] == '-')      { sign = -1; i = 1; }
    else if(s[0] == '+') { i = 1; }

    if(i >= len) return -1;   /* 只有符号，没有数字 */

    long v = 0;
    for(; i < len; i++) {
        if(s[i] < '0' || s[i] > '9') return -1;   /* 有一处非数字就整行作废 */
        v = v * 10 + (s[i] - '0');
        if(v > 1000000L) return -1;               /* 频率与 RSSI 都不可能到百万量级 */
    }

    *out = (int)(v * sign);
    return 0;
}

/* 是否表头行："bssid / frequency / signal / flags / ssid"。
 * 判据放宽到「以 bssid 起头且第一个字段里有 '/'」——不同版本表头措辞有出入，
 * 但 bssid 恒在第一列。 */
static int is_header(const char * b, const char * e)
{
    if(e - b < 5) return 0;
    if(memcmp(b, "bssid", 5) != 0) return 0;
    const char * first_tab = (const char *)memchr(b, '\t', (size_t)(e - b));
    const char * stop = first_tab ? first_tab : e;
    return memchr(b, '/', (size_t)(stop - b)) != NULL;
}

int wpa_parse_scan_results(const char * text, wpa_ap_t * out, size_t cap)
{
    if(text == NULL) return 0;
    if(out == NULL || cap == 0) return 0;

    const char * p = text;
    size_t n = 0;

    while(*p != '\0' && n < cap) {
        const char * eol = strchr(p, '\n');
        const char * end = eol ? eol : (p + strlen(p));

        /* 容忍 \r\n */
        if(end > p && end[-1] == '\r') end--;

        if(end > p && !is_header(p, end)) {
            /* 按 tab 切成 5 段：bssid freq signal flags ssid */
            const char * fb[4];
            size_t fl[4];
            const char * q = p;
            size_t filled = 0;

            while(filled < 4) {
                const char * t = (const char *)memchr(q, '\t', (size_t)(end - q));
                if(t == NULL) break;
                fb[filled] = q;
                fl[filled] = (size_t)(t - q);
                q = t + 1;
                filled++;
            }

            /* filled 必须为 4：少一个 tab 说明字段缺失，整行作废，
             * 不产出「一半字段」的残缺记录（上层据此判断的行数才可信）。 */
            if(filled == 4) {
                const char * ssid_b = q;
                size_t ssid_l      = (size_t)(end - q);

                int freq = 0, sig = 0;
                /* freq/signal 非法也整行作废：信号强度是本页唯一的排序依据，
                 * 给个 0 会让强弱失真。 */
                if(parse_int(fb[1], fl[1], &freq) == 0 &&
                   parse_int(fb[2], fl[2], &sig) == 0) {
                    wpa_ap_t * ap = &out[n];
                    memset(ap, 0, sizeof(*ap));
                    copy_field(fb[0], fb[0] + fl[0], ap->bssid, sizeof(ap->bssid));
                    copy_field(fb[3], fb[3] + fl[3], ap->flags, sizeof(ap->flags));
                    copy_field(ssid_b, ssid_b + ssid_l, ap->ssid, sizeof(ap->ssid));
                    ap->freq_mhz   = freq;
                    ap->signal_dbm = sig;
                    n++;
                }
            }
        }

        if(eol == NULL) break;
        p = eol + 1;
    }

    return (int)n;
}

int wpa_parse_list_networks(const char * text, net_saved_t * out, size_t cap)
{
    if(text == NULL) return 0;
    if(out == NULL || cap == 0) return 0;

    const char * p = text;
    size_t n = 0;

    while(*p != '\0' && n < cap) {
        const char * eol = strchr(p, '\n');
        const char * end = eol ? eol : (p + strlen(p));
        if(end > p && end[-1] == '\r') end--;

        if(end > p) {
            size_t len = (size_t)(end - p);
            /* 表头："network id / ssid / bssid / flags" */
            int is_head = (len >= 10 && memcmp(p, "network id", 10) == 0);

            if(!is_head) {
                /* 切前三个 tab：id / ssid / bssid，剩下的 flags 不用 */
                const char * fb[3];
                size_t fl[3];
                const char * q = p;
                size_t filled = 0;
                while(filled < 3) {
                    const char * t = (const char *)memchr(q, '\t', (size_t)(end - q));
                    if(t == NULL) break;
                    fb[filled] = q;
                    fl[filled] = (size_t)(t - q);
                    q = t + 1;
                    filled++;
                }

                int id = 0;
                if(filled == 3 && parse_int(fb[0], fl[0], &id) == 0) {
                    net_saved_t * s = &out[n];
                    memset(s, 0, sizeof(*s));
                    s->id = id;
                    /* SSID 为空是合法的（块建了但没填 SSID），照样产出，
                     * 让上层有机会把它清掉；否则这类僵尸配置肉眼看不见。 */
                    copy_field(fb[1], fb[1] + fl[1], s->ssid, sizeof(s->ssid));
                    n++;
                }
            }
        }

        if(eol == NULL) break;
        p = eol + 1;
    }

    return (int)n;
}

int wpa_status_get(const char * text, const char * key, char * out, size_t cap)
{
    if(text == NULL || key == NULL || out == NULL || cap == 0) return -2;

    size_t klen = strlen(key);
    if(klen == 0) return -2;

    out[0] = '\0';

    const char * p = text;
    while(*p != '\0') {
        const char * eol = strchr(p, '\n');
        const char * end = eol ? eol : (p + strlen(p));

        size_t len = (size_t)(end - p);
        if(len > 0 && p[len - 1] == '\r') len--;

        /* len > klen 保证 "=" 存在；p[klen]=='=' 排除 ssidx=1 这类前缀误命中 */
        if(len > klen && p[klen] == '=' && memcmp(p, key, klen) == 0) {
            const char * v  = p + klen + 1;
            size_t vl       = len - klen - 1;
            if(vl > cap - 1) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return 0;
        }

        if(eol == NULL) break;
        p = eol + 1;
    }

    return -4;
}

