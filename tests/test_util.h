/**
 * @file test_util.h
 * 极简断言工具：不引第三方框架，一个可执行文件一个 main。
 */
#pragma once

#include <stdio.h>

static int g_check_fail = 0;

#define CHECK(cond) do { \
    if(!(cond)) { \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_check_fail++; \
    } \
} while(0)

#define TEST_RESULT() do { \
    if(g_check_fail) { printf("[FAIL] %s（%d 项未过）\n", __FILE__, g_check_fail); return 1; } \
    printf("[PASS] %s\n", __FILE__); \
    return 0; \
} while(0)

/* 把 pin_hash 生成的 16 字节随机盐转成 hex（与 users.json 的 pin_salt 字段对齐） */
static void test_salt_to_hex(const unsigned char * salt, char * out)
{
    for(int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", salt[i]);
    out[32] = '\0';
}
