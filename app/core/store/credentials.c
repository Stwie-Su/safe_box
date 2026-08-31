/**
 * @file credentials.c
 * @brief 保险柜开锁密码的读取、校验与写入（password.cfg，认证加密存储）。
 *
 * 与多用户 PIN（store.c 管理 users.json）是两套体系，勿混淆：
 *   - 本文件  ：保险柜开锁密码（虚位密码，单一密钥）
 *   - store.c ：多用户 PIN（PBKDF2 哈希 + 随机盐）
 *
 * 密码文件路径统一取存储层数据目录（store_dir() + "password.cfg"），
 * 与 users.json / network.json / safe.log 落在同一处，便于整体备份与权限管理。
 */

#include "core/store/credentials.h"
#include "core/support/crypto.h"
#include "core/store/store.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <direct.h>    /* _mkdir()：Windows 创建目录 */
#else
  #include <unistd.h>
  #include <limits.h>
  #include <sys/stat.h>  /* mkdir() */
#endif

/**
 * @brief 获取可执行文件所在目录（不含结尾分隔符）
 *
 * 在 Windows 上通过 GetModuleFileNameA 获取模块路径，然后截取目录部分。
 * 在 Linux 上通过读取 /proc/self/exe 符号链接获取实际路径，再截取目录。
 *
 * @param buf  输出缓冲区
 * @param bufsz 缓冲区大小
 * @return const char* 成功返回 buf，失败返回 NULL
 */
static const char *get_exe_dir(char *buf, size_t bufsz) __attribute__((unused));
static const char *get_exe_dir(char *buf, size_t bufsz)
{
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    if (n == 0 || n >= bufsz) return NULL;
    buf[n] = '\0';
    char *p = buf + n;
    while (p > buf && p[-1] != '\\' && p[-1] != '/') p--;
    *p = '\0';
    return buf;
#else
    char link[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (n <= 0) return NULL;
    link[n] = '\0';
    char *p = link + n;
    while (p > link && p[-1] != '/') p--;
    *p = '\0';
    if ((size_t)(p - link) + 1 > bufsz) return NULL;
    memcpy(buf, link, (size_t)(p - link) + 1);   /* 含结尾 '\0' */
    return buf;
#endif
}

/**
 * @brief 构造密码文件的绝对路径
 * 规则：可执行文件目录的上级目录下的 src/safe/password.cfg。
 * 例如：bin/main -> 项目根/src/safe/password.cfg。
 * 如果无法获取可执行文件目录，则回退到编译时定义的 PASSWORD_FILE 宏。
 * @param out      输出缓冲区，存放完整路径
 * @param out_size 缓冲区大小
 */
static void password_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%spassword.cfg", store_dir());
}

/**
 * @brief 逐级创建指定文件路径的父目录
 *
 * 解析路径中的目录分隔符，依次创建各级目录（已存在则忽略错误）。
 * 入参为完整的文件路径（含文件名），函数会忽略最后一级（文件名本身）。
 * 支持 '/' 和 '\\' 作为分隔符。
 *
 * @param path 文件完整路径（用于提取目录部分）
 */
static void ensure_password_dir(const char *path)
{
    char dir[1024];
    size_t len = strlen(path);
    if (len >= sizeof(dir)) return;
    memcpy(dir, path, len + 1);

    char *p = dir;
    if (p[0] == '.' && p[1] == '/') p += 2;   /* 跳过开头的 ./ */
    for (char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char sep = *q;
            *q = '\0';
            if (strlen(dir) > 0) {
#ifdef _WIN32
                _mkdir(dir);
#else
                mkdir(dir, 0755);
#endif
            }
            *q = sep;
        }
    }
}

/**
 * @brief 获取当前有效密码（解密后的明文）
 * 优先从密码文件中读取加密数据并解密，若成功则返回明文。
 * 若文件不存在或解密失败，则使用默认密码 "123456"。
 * @param out      输出缓冲区，存放明文密码（以 '\0' 结尾）
 * @param out_size 输出缓冲区大小
 */
void config_get_password(char *out, size_t out_size)
{
    static const char *def = "123456";
    char path[1024];
    out[0] = '\0';

    password_path(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t blob[CRYPTO_BLOB_MAX];
        size_t n = fread(blob, 1, sizeof(blob), f);
        fclose(f);
        /* 解密成功即用；失败（旧明文文件/损坏）回退默认密码 */
        if (n > 0 && crypto_decrypt(blob, n, out, out_size)) {
            return;
        }
    }

    /* 文件不存在或解密失败：使用默认密码 */
    strncpy(out, def, out_size - 1);
    out[out_size - 1] = '\0';
}

/**
 * @brief 验证输入的密码是否与存储的密码一致
 *
 * @param input 待验证的密码字符串
 * @return true  密码正确
 * @return false 密码错误或输入为空
 */
bool config_verify_password(const char *input)
{
    char pwd[PIN_REAL_LEN + 1];

    if (input == NULL) return false;
    config_get_password(pwd, sizeof(pwd));
    return strcmp(input, pwd) == 0;
}

/**
 * @brief 设置新密码（加密后写入文件）
 *
 * 将新密码加密后写入密码文件。写入前会确保父目录存在。
 * 密码长度必须严格等于 PIN_REAL_LEN。
 *
 * @param new_pwd 新密码明文（长度必须为 PIN_REAL_LEN）
 * @return true  设置成功
 * @return false 参数无效或文件写入失败
 */
bool config_set_password(const char *new_pwd)
{
    char path[1024];

    if (new_pwd == NULL || strlen(new_pwd) != PIN_REAL_LEN) return false;

    password_path(path, sizeof(path));
    ensure_password_dir(path);          /* 先确保父目录存在，再写入 */

    uint8_t blob[CRYPTO_BLOB_MAX];
    size_t n = crypto_encrypt(new_pwd, blob, sizeof(blob));
    if (n == 0) return false;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    size_t w = fwrite(blob, 1, n, f);
    fclose(f);
    return w == n;
}