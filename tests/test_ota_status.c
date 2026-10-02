/**
 * @file test_ota_status.c
 * OTA 状态契约单测（D19 回归）：核心断言是「追加式写法下取最后一次匹配」。
 * 其余：重写式、未找到、参数非法、截断、CRLF、无尾换行、空文件、值含 '='、
 * 前缀相似键不误匹配。
 */
#include <stdio.h>
#include <string.h>

#include "core/support/ota_status.h"

static int fails = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while(0)

int main(void)
{
    char v[80];

    /* 1. 追加式（历史写法）：同 KEY 多行取最后 —— D19 根因回归 */
    CHECK(ota_status_query("STATE=CHECKING\nSTATE=CHECKED\n", "STATE", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "CHECKED") == 0);

    /* 2. 重写式（现行写法） */
    CHECK(ota_status_query("STATE=CHECKED\nSRC=HTTP\n", "STATE", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "CHECKED") == 0);
    CHECK(ota_status_query("STATE=CHECKED\nSRC=HTTP\n", "SRC", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "HTTP") == 0);

    /* 3. 未找到 / 参数非法 */
    CHECK(ota_status_query("STATE=CHECKED\n", "PROGRESS", v, sizeof(v)) == -4);
    CHECK(ota_status_query(NULL, "STATE", v, sizeof(v)) == -2);
    CHECK(ota_status_query("STATE=CHECKED\n", "STATE", NULL, 10) == -2);
    CHECK(ota_status_query("STATE=CHECKED\n", "STATE", v, 0) == -2);

    /* 4. 截断：cap=5 时 "CHECKED"(7B) 截为 4 字节 + NUL，且返回 0 */
    CHECK(ota_status_query("STATE=CHECKED\n", "STATE", v, 5) == 0);
    CHECK(strlen(v) == 4);

    /* 5. CRLF 容忍 */
    CHECK(ota_status_query("STATE=INSTALL\r\nSRC=HTTP\r\n", "SRC", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "HTTP") == 0);

    /* 6. 无尾换行 */
    CHECK(ota_status_query("STATE=RESTARTING", "STATE", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "RESTARTING") == 0);

    /* 7. 空文件 */
    CHECK(ota_status_query("", "STATE", v, sizeof(v)) == -4);

    /* 8. 值内含 '='：键前缀匹配后整段为值 */
    CHECK(ota_status_query("SHA256=abc=def\n", "SHA256", v, sizeof(v)) == 0);
    CHECK(strcmp(v, "abc=def") == 0);

    /* 9. 前缀相似键不误匹配（STATE_X 不是 STATE） */
    CHECK(ota_status_query("STATE_X=1\n", "STATE", v, sizeof(v)) == -4);

    if(fails == 0) { printf("test_ota_status: ALL PASS\n"); return 0; }
    printf("test_ota_status: %d FAILS\n", fails);
    return 1;
}
