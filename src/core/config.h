/**
 * @file config.h
 * 配置接口：密码的读取、校验与写入（管理密码持久化）。
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>

#define PIN_REAL_LEN 6      /* 真实密码长度 */
#define PIN_MAX_LEN  20     /* 虚位密码最大输入长度 */
#define LOCK_ATTEMPTS 5     /* 连续错误次数上限 */
#define LOCK_SECONDS  30    /* 锁定时长（秒） */

/* 密码文件路径（项目根 src/safe/password.cfg，加密存储） */
#define PASSWORD_FILE "src/safe/password.cfg"

/* 读取真实密码到 out（长度 >= PIN_REAL_LEN+1）。文件不存在或读空时返回默认 "123456"。 */
void config_get_password(char *out, size_t out_size);

/* 校验输入是否等于当前密码（精确匹配，供修改密码时验证旧密码）。 */
bool config_verify_password(const char *input);

/* 设置新密码（长度必须等于 PIN_REAL_LEN），写入密码文件，成功返回 true。 */
bool config_set_password(const char *new_pwd);
