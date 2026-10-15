/**
 * @file ota_status.c
 * OTA 状态接口约定解析（纯函数：无 IO、无堆分配、不改入参）。
 * 接口约定见 ota_status.h 头注释；行为由 tests/test_ota_status.c 确定性锁定。
 */
#include "core/support/ota_status.h"

#include <string.h>

int ota_status_query(const char * buf, const char * key, char * out, size_t cap)
{
    if(buf == NULL || key == NULL || out == NULL || cap == 0) return -2;

    size_t klen = strlen(key);
    int found = 0;
    out[0] = '\0';

    const char * p = buf;
    while(p != NULL && *p != '\0') {
        const char * eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if(len > 0 && p[len - 1] == '\r') len--;          /* 容忍 CRLF */

        if(len > klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = len - klen - 1;
            if(vlen >= cap) vlen = cap - 1;                /* 宁截断不报错 */
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            found = 1;                                     /* 继续找：取最后匹配 */
        }
        p = eol ? eol + 1 : NULL;
    }
    return found ? 0 : -4;
}
