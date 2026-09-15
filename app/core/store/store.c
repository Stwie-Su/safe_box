/**
 * @file store.c
 * 数据存储实现（DESIGN.md §4）：JSON 文件 + 抽象存储层。
 *
 * 三份文件（DESIGN.md §4.2）：
 *   users.json    —— 多用户 / 角色 / PIN 哈希 / 盐 / 防暴力计数（只存哈希）
 *   network.json  —— SSID / 加密方式 / psk_enc（经 crypto 认证加密后 hex 存储）
 *   safe.log      —— 审计日志，JSON Lines 明文追加，滚动截断
 *
 * 内置极简 JSON 读写器（仅服务于本模块固定 schema，不做通用库）。
 */
#include "core/store/store.h"
#include "core/config.h"       /* 运行期配置快照（SAFE_LOG_MAX 等） */
#include "core/support/crypto.h"
#include "hal/hal_time.h"        /* R2：时间源统一走 HAL，业务层不直接读系统时钟 */
#include "app_version.h"          /* SAFE_DATA_DIR：编译期数据目录 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
  #include <direct.h>
  #include <io.h>
  #define MKDIR(p) _mkdir(p)
  #define localtime_r(tp, tmp) localtime_s(tmp, tp)
  #define fileno(f) _fileno(f)      /* 原子写需要的文件描述符 / 落盘原语（MSVC 命名） */
  #define fsync(fd) _commit(fd)
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <limits.h>
  #include <sys/stat.h>
  #include <pthread.h>
  #define MKDIR(p) mkdir(p, 0755)
#endif

/* ---------------- 常量 ---------------- */
#define PBKDF2_ITER_USER  10000u   /* PIN 哈希迭代次数 */
/* 日志滚动保留条数：默认值单一来源于 config 层（APP_CFG_LOG_MAX_DEFAULT），
 * 运行期由 SAFE_LOG_MAX 覆盖。D6：原先此处硬编码 500，导致 app_config 里的
 * log_max_entries / SAFE_LOG_MAX 形同虚设。 */
static int log_retention_max(void)
{
    int m = app_config()->log_max_entries;
    return (m > 0) ? m : APP_CFG_LOG_MAX_DEFAULT;
}

#define USERS_FILE  "users.json"
#define NET_FILE    "network.json"
#define LOG_FILE    "safe.log"

/* 默认管理员：首次运行创建（DESIGN.md §2.2 示例用户） */
#define BOOT_USER_NAME  "admin"
#define BOOT_USER_PIN   "123456"

/* ---------------- 全局 ---------------- */
static char g_dir[512] = {0};          /* 数据目录（store_set_dir 覆盖） */
static safe_policy_t g_policy = { 4, 8, 5, 30, 3, 10, true };
/* pin_min/max/max_failed/lock_seconds/face_otp_after/face_verify_timeout_s/virtual_pin_enable */

/* DESIGN.md §9：g_policy 被主线程(user_policy)与后台 worker(user_policy_set/load_users)
 * 交叉访问，加一把小锁；文件读写仍由 worker 单线程持有，无需全局锁。 */
static pthread_mutex_t g_policy_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 默认数据目录：
 *   编译期由 SAFE_DATA_DIR 决定（PC = 工程内 data/，板子 = /var/lib/safe），
 *   运行时可用环境变量 SAFE_DATA_DIR 覆盖。
 *   源码目录不再产生任何运行时文件。 */
static const char *default_dir(void)
{
    static char dir[512];
    static int  done = 0;
    if(!done) {
        const char * env  = getenv("SAFE_DATA_DIR");
        const char * base = (env && *env) ? env : SAFE_DATA_DIR;
        snprintf(dir, sizeof(dir), "%s", base);
        size_t len = strlen(dir);
        if(len > 0 && dir[len - 1] != '/' && len + 1 < sizeof(dir)) {
            dir[len]     = '/';
            dir[len + 1] = '\0';
        }
        done = 1;
    }
    return dir;
}

const char * store_dir(void)
{
    return (g_dir[0] ? g_dir : default_dir());
}

void store_set_dir(const char *dir)
{
    if (dir) {
        strncpy(g_dir, dir, sizeof(g_dir) - 2);
        g_dir[sizeof(g_dir) - 1] = '\0';
        /* 规范化：保证以 '/' 结尾，拼接文件名时不依赖调用方格式 */
        size_t len = strlen(g_dir);
        if (len > 0 && g_dir[len - 1] != '/') {
            g_dir[len]     = '/';
            g_dir[len + 1] = '\0';
        }
    } else {
        g_dir[0] = '\0';
    }
}

/* 确保数据目录存在 */
static void ensure_dir(const char *dir)
{
    char tmp[512];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, dir, len + 1);
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            if (tmp[0]) MKDIR(tmp);
            *p = sep;
        }
    }
}

/* 拼接 dir + file 到 out */
static void file_path(const char *fname, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", store_dir(), fname);
}

/* 读取整个文件到堆（含 \0）。不存在返回 NULL。 */
static char *read_file_alloc(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* fsync 父目录：rename 只改了内存里的目录项，目录本身不落盘的话掉电后可能
 * 既看不到新文件也看不到旧文件。POSIX 要求显式同步父目录才能保住 rename。 */
static void fsync_parent_dir(const char *path)
{
#if defined(_WIN32)
    (void)path;                      /* Windows 没有目录 fsync 语义，保持空实现 */
#else
    char dup[600];
    size_t n = strlen(path);
    if(n == 0 || n >= sizeof(dup)) return;
    memcpy(dup, path, n + 1);
    char *slash = strrchr(dup, '/');
    if(!slash) return;               /* 没有目录成分，无法定位父目录 */
    if(slash == dup) dup[1] = '\0';  /* "/xxx" 的父目录是根 "/" */
    else            *slash = '\0';
    int fd = open(dup, O_RDONLY);
    if(fd < 0) return;
    fsync(fd);
    close(fd);
#endif
}

/* 原子替换写：tmp → fsync → rename → fsync(父目录)。
 *
 * 为什么不能直接 fwrite 覆盖：覆盖写存在一个"只写了一半"的时间窗，掉电或进程
 * 崩溃后 users.json 会变成截断的 JSON，全库解析失败等于全员无法开锁
 * （技术路线规约 §10 风险项 R1，最高优先）。
 *
 * rename 在同一文件系统内是原子的：任意时刻读目标路径，要么拿到完整旧内容，
 * 要么拿到完整新内容，没有中间态。两次 fsync 分别保证"数据落盘"与"目录项落盘"，
 * 缺任何一个都保不住 rename 的结果。
 *
 * users.json 与 network.json 共用本函数；落盘由后台 worker 串行调用，
 * 因此临时文件名可以用固定的 .tmp 后缀（崩溃残留会在下次写时被覆盖）。 */
static bool write_file_all(const char *path, const char *data, size_t len)
{
    if(!path || !data) return false;

    char tmp[600];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if(n < 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *f = fopen(tmp, "wb");
    if(!f) return false;

    bool ok = (fwrite(data, 1, len, f) == len);
    /* 顺序不能变：先 flush 到内核，再 fsync 落盘，最后关闭。
     * 只 fclose 的话数据还留在页缓存里，掉电即丢。 */
    if(ok && fflush(f) != 0) ok = false;
    if(ok && fsync(fileno(f)) != 0) ok = false;
    if(fclose(f) != 0) ok = false;

    if(!ok) { remove(tmp); return false; }
    if(rename(tmp, path) != 0) { remove(tmp); return false; }
    fsync_parent_dir(path);
    return true;
}

/* ==================================================================
 * 极简 JSON 读写器（对象/数组/字符串/数字/布尔；不支持浮点）
 * ================================================================== */
static const char *js_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* 解析 "..."（含 \ 转义，\u 简化为 '?'）。返回结束引号后的位置；失败 NULL。 */
static const char *js_str(const char *p, char *out, size_t cap)
{
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n + 1 >= cap) return NULL;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  out[n++] = '"';  break;
                case '\\': out[n++] = '\\'; break;
                case '/':  out[n++] = '/';  break;
                case 'b':  out[n++] = '\b'; break;
                case 'f':  out[n++] = '\f'; break;
                case 'n':  out[n++] = '\n'; break;
                case 'r':  out[n++] = '\r'; break;
                case 't':  out[n++] = '\t'; break;
                case 'u':  p += 4; out[n++] = '?'; break;
                default:   if (!*p) return NULL; out[n++] = *p; break;
            }
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    if (*p != '"') return NULL;
    out[n] = '\0';
    return p + 1;
}

/* 跳过任意一个值，返回其后的位置 */
static const char *js_skip(const char *p)
{
    p = js_ws(p);
    if (*p == '"') {
        /* 字符串：直接扫描到未转义的结束引号（不复制内容） */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return (*p == '"') ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        p++;
        while (*p) {
            if (*p == open) depth++;
            else if (*p == close) {
                if (depth == 0) return p + 1;
                depth--;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p++;
                    p++;
                }
                if (!*p) return NULL;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 在对象中按 key 找 value 起始位置（key 需为字符串） */
static const char *js_get(const char *obj, const char *key)
{
    const char *p = obj;
    while (p && *p) {
        p = js_ws(p);
        if (*p != '"') { p++; continue; }
        char kbuf[48];
        p = js_str(p, kbuf, sizeof(kbuf));
        if (!p) return NULL;
        p = js_ws(p);
        if (*p != ':') { p++; continue; }
        p = js_ws(p + 1);
        if (strcmp(kbuf, key) == 0) return p;
        p = js_skip(p);
        if (!p) return NULL;
        if (*p == ',') p++;
    }
    return NULL;
}

static bool js_get_str(const char *obj, const char *key, char *out, size_t cap)
{
    const char *v = js_get(obj, key);
    if (!v || *v != '"') return false;
    return js_str(v, out, cap) != NULL;
}

static bool js_get_int(const char *obj, const char *key, long *out)
{
    const char *v = js_get(obj, key);
    if (!v) return false;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return false;
    *out = n;
    return true;
}

static bool js_get_bool(const char *obj, const char *key, bool *out)
{
    const char *v = js_get(obj, key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* 数组：[...]，返回第 idx 个元素起始位置（idx 从 0）；不存在返回 NULL */
static const char *js_arr_at(const char *arr, int idx)
{
    const char *p = arr;
    int cur = 0;
    while (p && *p) {
        p = js_ws(p);
        if (*p == '[') { p++; continue; }
        if (*p == ']') return NULL;
        if (cur == idx) return p;
        p = js_skip(p);
        if (!p) return NULL;     /* 截断的 JSON：js_skip 扫不到闭合符会返回 NULL，必须挡住再解引用 */
        if (*p == ',') p++;
        cur++;
    }
    return NULL;
}

/* 数组元素个数 */
static int js_arr_count(const char *arr)
{
    const char *p = arr;
    int n = 0;
    while (p && *p) {
        p = js_ws(p);
        if (*p == '[') { p++; continue; }
        if (*p == ']') return n;
        p = js_skip(p);
        if (!p) return n;        /* 同上：文件被截断时按已数到的个数返回，由上层按"条数不符"判坏 */
        if (*p == ',') p++;
        n++;
    }
    return n;
}

/* 生成端：字符串转义后写入 "..." */
static void js_quote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (o < cap) out[o++] = '"';
    for (const char *q = s; *q && o + 1 < cap; q++) {
        switch (*q) {
            case '"':  if (o + 2 < cap) { out[o++] = '\\'; out[o++] = '"'; } break;
            case '\\': if (o + 2 < cap) { out[o++] = '\\'; out[o++] = '\\'; } break;
            case '\n': if (o + 2 < cap) { out[o++] = '\\'; out[o++] = 'n'; } break;
            case '\t': if (o + 2 < cap) { out[o++] = '\\'; out[o++] = 't'; } break;
            case '\r': if (o + 2 < cap) { out[o++] = '\\'; out[o++] = 'r'; } break;
            default:   out[o++] = *q; break;
        }
    }
    if (o + 1 < cap) out[o++] = '"';
    out[o] = '\0';
}

/* ==================================================================
 * hex 编解码
 * ================================================================== */
static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = D[in[i] >> 4];
        out[i * 2 + 1] = D[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int from_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > cap) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int h = hex_nibble(hex[i * 2]);
        int l = hex_nibble(hex[i * 2 + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return (int)(len / 2);
}

/* ==================================================================
 * PIN 哈希（DESIGN.md §2.3：PBKDF2-SHA256，只存哈希）
 * ================================================================== */
int pin_hash(const char *pin, uint8_t *salt_out, char *hash_hex_out)
{
    if (!pin || !*pin) return -1;
    uint8_t salt[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        (void)fread(salt, 1, sizeof(salt), f);
        fclose(f);
    } else {
        srand((unsigned)hal_time());
        for (size_t i = 0; i < sizeof(salt); i++) salt[i] = (uint8_t)rand();
    }
    uint8_t dk[32];
    crypto_pbkdf2_sha256((const uint8_t *)pin, strlen(pin), salt, sizeof(salt),
                         PBKDF2_ITER_USER, dk, sizeof(dk));
    if (salt_out) memcpy(salt_out, salt, sizeof(salt));
    if (hash_hex_out) to_hex(dk, sizeof(dk), hash_hex_out);
    return 0;
}

int pin_check(const char *pin, const char *salt_hex, const char *hash_hex)
{
    if (!pin || !salt_hex || !hash_hex) return -1;
    uint8_t salt[16];
    if (from_hex(salt_hex, salt, sizeof(salt)) != (int)sizeof(salt)) return -1;
    uint8_t dk[32], exp[32];
    if (from_hex(hash_hex, exp, sizeof(exp)) != (int)sizeof(exp)) return -1;
    crypto_pbkdf2_sha256((const uint8_t *)pin, strlen(pin), salt, sizeof(salt),
                         PBKDF2_ITER_USER, dk, sizeof(dk));
    return memcmp(dk, exp, sizeof(dk)) == 0 ? 0 : -1;
}

/* ==================================================================
 * users.json 读写
 * ================================================================== */
static void user_to_json(const safe_user_t *u, char *out, size_t cap)
{
    char nq[96], rq[48], amq[48], cq[48], tsq[48];
    js_quote(u->name,  nq,  sizeof(nq));
    js_quote(u->role,  rq,  sizeof(rq));
    js_quote(u->auth_method, amq, sizeof(amq));
    js_quote(u->created_at,  cq,  sizeof(cq));
    js_quote(u->totp_secret, tsq, sizeof(tsq));
    snprintf(out, cap,
        "{\"id\":%d,\"name\":%s,\"role\":%s,\"pin_hash\":\"%s\",\"pin_salt\":\"%s\","
        "\"auth_method\":%s,\"enabled\":%s,\"created_at\":%s,\"failed_attempts\":%d,\"lock_until\":%lld,"
        "\"face_id\":%d,\"face_enable\":%s,\"totp_enable\":%s,\"totp_secret\":%s,"
        "\"last_otp_counter\":%lld,\"valid_until\":%lld,\"use_limit\":%d,\"used_count\":%d}",
        u->id, nq, rq, u->pin_hash, u->pin_salt, amq,
        u->enabled ? "true" : "false", cq, u->failed_attempts, (long long)u->lock_until,
        u->face_id, u->face_enable ? "true" : "false", u->totp_enable ? "true" : "false", tsq,
        (long long)u->last_otp_counter, (long long)u->valid_until, u->use_limit, u->used_count);
}

static bool parse_user_obj(const char *obj, safe_user_t *u)
{
    long v;
    memset(u, 0, sizeof(*u));
    u->face_id = -1;            /* 老数据缺字段时的安全默认值 */
    if (!js_get_int(obj, "id", &v)) return false;
    u->id = (int)v;
    if (!js_get_str(obj, "name", u->name, sizeof(u->name))) return false;
    if (!js_get_str(obj, "role", u->role, sizeof(u->role))) return false;
    if (!js_get_str(obj, "pin_hash", u->pin_hash, sizeof(u->pin_hash))) return false;
    if (!js_get_str(obj, "pin_salt", u->pin_salt, sizeof(u->pin_salt))) return false;
    if (!js_get_str(obj, "auth_method", u->auth_method, sizeof(u->auth_method))) return false;
    js_get_bool(obj, "enabled", &u->enabled);
    js_get_str(obj, "created_at", u->created_at, sizeof(u->created_at));
    if (js_get_int(obj, "failed_attempts", &v)) u->failed_attempts = (int)v;
    if (js_get_int(obj, "lock_until", &v)) u->lock_until = v;
    /* —— 阶段 1 新增字段：缺省给兼容默认值 —— */
    if (js_get_int(obj, "face_id", &v)) u->face_id = (int)v; else u->face_id = -1;
    js_get_bool(obj, "face_enable", &u->face_enable);
    js_get_bool(obj, "totp_enable", &u->totp_enable);
    js_get_str(obj, "totp_secret", u->totp_secret, sizeof(u->totp_secret));
    if (js_get_int(obj, "last_otp_counter", &v)) u->last_otp_counter = v;
    if (js_get_int(obj, "valid_until", &v)) u->valid_until = v;
    if (js_get_int(obj, "use_limit", &v)) u->use_limit = (int)v;
    if (js_get_int(obj, "used_count", &v)) u->used_count = (int)v;
    return true;
}

/* 读 users.json：返回 malloc 的用户数组与个数。文件缺失返回 0/0。 */
static int load_users(safe_user_t **list, int *count, bool *ok)
{
    *list = NULL;
    *count = 0;
    if (ok) *ok = true;
    char path[560];
    file_path(USERS_FILE, path, sizeof(path));
    char *json = read_file_alloc(path);
    if (!json) return 0;

    /* 同步 policy（若文件中有） */
    const char *pol = js_get(json, "policy");
    if (pol) {
        long v;
        pthread_mutex_lock(&g_policy_mutex);
        if (js_get_int(pol, "pin_min_len", &v) && v >= 4) g_policy.pin_min_len = (int)v;
        if (js_get_int(pol, "pin_max_len", &v) && v >= 4) g_policy.pin_max_len = (int)v;
        if (js_get_int(pol, "max_failed", &v) && v >= 1)  g_policy.max_failed = (int)v;
        if (js_get_int(pol, "lock_seconds", &v) && v >= 1) g_policy.lock_seconds = (int)v;
        /* 阈值有合理范围才采纳；旧版 score_high/score_mid 字段直接忽略（兼容读取） */
        if (js_get_int(pol, "face_otp_after", &v) && v >= 1 && v <= 10) g_policy.face_otp_after = (int)v;
        if (js_get_int(pol, "face_verify_timeout_s", &v) && v >= 3 && v <= 120) g_policy.face_verify_timeout_s = (int)v;
        pthread_mutex_unlock(&g_policy_mutex);
    }

    const char *arr = js_get(json, "users");
    if (!arr) { free(json); if (ok) *ok = false; return 0; }
    int n = js_arr_count(arr);
    if (n <= 0) { free(json); return 0; }

    safe_user_t *us = calloc((size_t)n, sizeof(safe_user_t));
    if (!us) { free(json); if (ok) *ok = false; return 0; }
    for (int i = 0; i < n; i++) {
        const char *uobj = js_arr_at(arr, i);
        if (!uobj || !parse_user_obj(uobj, &us[i])) { if (ok) *ok = false; }
    }
    free(json);
    *list = us;
    *count = n;
    return 0;
}

static bool save_users(const safe_user_t *us, int n)
{
    char path[560];
    file_path(USERS_FILE, path, sizeof(path));
    ensure_dir(store_dir());

    /* 估算容量：每用户 ~512 字节 */
    size_t cap = 512 + (size_t)n * 640;
    char *buf = malloc(cap);
    if (!buf) return false;
    size_t o = 0;
    const safe_policy_t *p = &g_policy;
    o += (size_t)snprintf(buf + o, cap - o,
        "{\n  \"version\": 1,\n  \"policy\": {\"pin_min_len\": %d, \"pin_max_len\": %d, "
        "\"max_failed\": %d, \"lock_seconds\": %d, \"face_otp_after\": %d, "
        "\"face_verify_timeout_s\": %d, \"virtual_pin_enable\": %s},\n  \"users\": [\n",
        p->pin_min_len, p->pin_max_len, p->max_failed, p->lock_seconds,
        p->face_otp_after, p->face_verify_timeout_s, p->virtual_pin_enable ? "true" : "false");
    for (int i = 0; i < n; i++) {
        char one[600];
        user_to_json(&us[i], one, sizeof(one));
        o += (size_t)snprintf(buf + o, cap - o, "    %s%s\n", one, (i < n - 1) ? "," : "");
    }
    o += (size_t)snprintf(buf + o, cap - o, "  ]\n}\n");
    bool r = write_file_all(path, buf, o);
    free(buf);
    return r;
}

int user_load_all(safe_user_t **list, int *count)
{
    bool ok = true;
    load_users(list, count, &ok);
    return ok ? 0 : -1;
}

void user_list_free(safe_user_t *list)
{
    free(list);
}

int user_find_by_name(const char *name, safe_user_t *out)
{
    if (!name) return -1;
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].name, name) == 0) {
            if (out) *out = us[i];
            user_list_free(us);
            return 0;
        }
    }
    user_list_free(us);
    return -1;
}

int user_find_by_id(int id, safe_user_t *out)
{
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (us[i].id == id) {
            if (out) *out = us[i];
            user_list_free(us);
            return 0;
        }
    }
    user_list_free(us);
    return -1;
}

int user_find_by_face(int face_id, safe_user_t *out)
{
    if (face_id < 0) return -1;   /* 陌生人无法映射 */
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (us[i].face_id == face_id) {
            if (out) *out = us[i];
            user_list_free(us);
            return 0;
        }
    }
    user_list_free(us);
    return -1;
}

int user_next_id(void)
{
    safe_user_t *us = NULL;
    int n = 0, maxid = 0;
    if (load_users(&us, &n, NULL) == 0) {
        for (int i = 0; i < n; i++) if (us[i].id > maxid) maxid = us[i].id;
        user_list_free(us);
    }
    return maxid + 1;
}

int user_add(const safe_user_t *u)
{
    if (!u || !u->name[0] || !u->pin_hash[0]) return -1;
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);

    /* 重名检查 */
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].name, u->name) == 0) { user_list_free(us); return -2; }
    }
    safe_user_t *nu = realloc(us, (size_t)(n + 1) * sizeof(safe_user_t));
    if (!nu) { user_list_free(us); return -1; }
    nu[n] = *u;
    bool ok = save_users(nu, n + 1);
    user_list_free(nu);
    return ok ? 0 : -1;
}

int user_del(int id)
{
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) { user_list_free(us); return -1; }

    /* 硬约束（需求 §2.2）：删除后必须仍保留至少 1 个启用状态的管理员，
     * 防止系统被锁死。目标不存在或会掏空管理员时拒绝。 */
    int target_idx = -1;
    for (int i = 0; i < n; i++) {
        if (us[i].id == id) { target_idx = i; break; }
    }
    if (target_idx < 0) { user_list_free(us); return -1; }   /* 未找到 */
    if (strcmp(us[target_idx].role, "admin") == 0 && us[target_idx].enabled) {
        int admins_left = 0;
        for (int i = 0; i < n; i++) {
            if (i != target_idx &&
                strcmp(us[i].role, "admin") == 0 && us[i].enabled) {
                admins_left++;
            }
        }
        if (admins_left == 0) {
            user_list_free(us);
            log_append("user_del", us[target_idx].name, 0, "拒绝：需保留至少一个启用管理员");
            return -1;
        }
    }

    int keep = 0;
    for (int i = 0; i < n; i++) {
        if (us[i].id != id) us[keep++] = us[i];
    }
    bool ok = save_users(us, keep);
    user_list_free(us);
    return ok ? 0 : -1;
}

int user_update(const safe_user_t *u)
{
    if (!u) return -1;
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) { user_list_free(us); return -1; }
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (us[i].id == u->id) { us[i] = *u; found = true; break; }
    }
    if (!found) { user_list_free(us); return -1; }
    bool ok = save_users(us, n);
    user_list_free(us);
    return ok ? 0 : -1;
}

/* 人脸模板绑定单字段写（规约 §5.5 / UI 现代化 ui3）。
 * 读整表 → 定位 → 改 face_id → 整表原子落盘，读改写收在 store 一处，
 * 避免调用方「查→改→写回」三步在并发写下丢失更新。
 * face_id >= 0 时顺带把其它用户身上相同的模板号清成 -1：FM225 一个模板只属于
 * 一个用户，防御性去重保证 user_find_by_face 的映射保持唯一。 */
int user_face_set(int user_id, int face_id)
{
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) { user_list_free(us); return -1; }

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (us[i].id == user_id) { idx = i; break; }
    }
    if (idx < 0) { user_list_free(us); return -1; }   /* 用户不存在 */

    if (face_id >= 0) {
        for (int i = 0; i < n; i++) {
            if (i != idx && us[i].face_id == face_id) us[i].face_id = -1;
        }
    }
    us[idx].face_id = face_id;
    /* 绑定/重绑有效模板号（face_id >= 0）时顺带启用该用户的人脸通道 —— 这是「凭据
     * 恢复」路径。face_enable 会被三条通道置 false：auth_fsm 的名字核对不符
     * （FR-21 防线 1）、启动对账标孤儿（FR-21 防线 3）、管理员手动关闭。若此处只写
     * face_id 不写 face_enable，用户重新录入人脸后 resolve() 仍会因 face_enable==false
     * 永久拒绝（QA 复核 高危#2：真机复录后无法人脸开锁）。
     * face_id < 0（清除绑定）时不动 face_enable：不与「管理员开关」语义互相覆盖。 */
    if (face_id >= 0) us[idx].face_enable = true;

    bool ok = save_users(us, n);
    user_list_free(us);
    return ok ? 0 : -1;
}

const safe_policy_t * user_policy(void)
{
    /* 返回静态快照，避免调用者持有 &g_policy 与后台写入竞争 */
    static safe_policy_t snap;
    pthread_mutex_lock(&g_policy_mutex);
    snap = g_policy;
    pthread_mutex_unlock(&g_policy_mutex);
    return &snap;
}

void user_policy_set(int max_failed, int lock_seconds)
{
    pthread_mutex_lock(&g_policy_mutex);
    if (max_failed >= 1 && max_failed <= 10) g_policy.max_failed = max_failed;
    if (lock_seconds >= 5 && lock_seconds <= 3600) g_policy.lock_seconds = lock_seconds;
    pthread_mutex_unlock(&g_policy_mutex);
    /* 落盘：读出全部用户后以新策略重写 users.json */
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);
    if (n > 0) save_users(us, n);
    user_list_free(us);
}

void user_policy_set_face(int otp_after, int timeout_s)
{
    pthread_mutex_lock(&g_policy_mutex);
    if (otp_after >= 1 && otp_after <= 10)      g_policy.face_otp_after = otp_after;
    if (timeout_s >= 3  && timeout_s <= 120)    g_policy.face_verify_timeout_s = timeout_s;
    pthread_mutex_unlock(&g_policy_mutex);
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);
    if (n > 0) save_users(us, n);
    user_list_free(us);
}

/* 校验 PIN 并更新防暴力计数（DESIGN.md §2.3）：
 * 0=通过 1=错误(未锁定) 2=已锁定 -1=无此用户 */
int user_verify_pin(const char *name, const char *pin)
{
    if (!name || !pin) return -1;
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) { user_list_free(us); return -1; }

    long now = (long)hal_time();
    int ret = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(us[i].name, name) != 0) continue;
        if (!us[i].enabled) { ret = -1; break; }
        if (us[i].lock_until > now) { ret = 2; break; }

        if (pin_check(pin, us[i].pin_salt, us[i].pin_hash) == 0) {
            us[i].failed_attempts = 0;
            save_users(us, n);
            ret = 0;
        } else {
            us[i].failed_attempts++;
            if (us[i].failed_attempts >= g_policy.max_failed) {
                us[i].lock_until = now + g_policy.lock_seconds;
                us[i].failed_attempts = 0;
            }
            save_users(us, n);
            ret = 1;
        }
        break;
    }
    user_list_free(us);
    return ret;
}

/* ==================================================================
 * network.json（psk 可逆加密存储）
 * ================================================================== */
typedef struct {
    char ssid[64];
    char sec[16];
    char psk_enc[400];   /* hex blob */
} net_entry_t;

static int load_nets(net_entry_t **list, int *count)
{
    *list = NULL;
    *count = 0;
    char path[560];
    file_path(NET_FILE, path, sizeof(path));
    char *json = read_file_alloc(path);
    if (!json) return 0;

    const char *arr = js_get(json, "networks");
    if (!arr) { free(json); return 0; }
    int n = js_arr_count(arr);
    if (n <= 0) { free(json); return 0; }
    net_entry_t *ns = calloc((size_t)n, sizeof(net_entry_t));
    if (!ns) { free(json); return -1; }
    for (int i = 0; i < n; i++) {
        const char *o = js_arr_at(arr, i);
        if (!o) continue;
        js_get_str(o, "ssid", ns[i].ssid, sizeof(ns[i].ssid));
        js_get_str(o, "sec", ns[i].sec, sizeof(ns[i].sec));
        js_get_str(o, "psk_enc", ns[i].psk_enc, sizeof(ns[i].psk_enc));
    }
    free(json);
    *list = ns;
    *count = n;
    return 0;
}

static bool save_nets(const net_entry_t *ns, int n)
{
    char path[560];
    file_path(NET_FILE, path, sizeof(path));
    ensure_dir(store_dir());
    size_t cap = 256 + (size_t)n * 640;
    char *buf = malloc(cap);
    if (!buf) return false;
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "{\n  \"version\": 1,\n  \"networks\": [\n");
    for (int i = 0; i < n; i++) {
        char sq[160], seq[64];
        js_quote(ns[i].ssid, sq, sizeof(sq));
        js_quote(ns[i].sec,  seq, sizeof(seq));
        o += (size_t)snprintf(buf + o, cap - o,
            "    {\"ssid\":%s,\"sec\":%s,\"psk_enc\":\"%s\"}%s\n",
            sq, seq, ns[i].psk_enc, (i < n - 1) ? "," : "");
    }
    o += (size_t)snprintf(buf + o, cap - o, "  ]\n}\n");
    bool r = write_file_all(path, buf, o);
    free(buf);
    return r;
}

int net_add_wifi(const char *ssid, const char *sec, const char *psk)
{
    if (!ssid || !*ssid) return -1;
    uint8_t blob[CRYPTO_BLOB_MAX];
    size_t bn = crypto_encrypt(psk ? psk : "", blob, sizeof(blob));
    if (bn == 0) return -1;

    net_entry_t *ns = NULL;
    int n = 0;
    load_nets(&ns, &n);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(ns[i].ssid, ssid) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        net_entry_t *nn = realloc(ns, (size_t)(n + 1) * sizeof(net_entry_t));
        if (!nn) { free(ns); return -1; }
        ns = nn;
        memset(&ns[n], 0, sizeof(ns[n]));
        idx = n;
        n++;
    }
    strncpy(ns[idx].ssid, ssid, sizeof(ns[idx].ssid) - 1);
    strncpy(ns[idx].sec, sec ? sec : "WPA2", sizeof(ns[idx].sec) - 1);
    to_hex(blob, bn, ns[idx].psk_enc);

    bool ok = save_nets(ns, n);
    free(ns);
    return ok ? 0 : -1;
}

int net_get_psk(const char *ssid, char *psk_out, size_t cap)
{
    if (!ssid || !psk_out) return -1;
    net_entry_t *ns = NULL;
    int n = 0;
    load_nets(&ns, &n);
    int r = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(ns[i].ssid, ssid) == 0) {
            uint8_t blob[CRYPTO_BLOB_MAX];
            int bn = from_hex(ns[i].psk_enc, blob, sizeof(blob));
            if (bn > 0 && crypto_decrypt(blob, (size_t)bn, psk_out, cap)) r = 0;
            break;
        }
    }
    free(ns);
    return r;
}

/* ==================================================================
 * safe.log（JSON Lines，追加 + 滚动截断）
 * ================================================================== */
int log_append(const char *evt, const char *user, int res, const char *detail)
{
    if (!evt) return -1;
    char path[560];
    file_path(LOG_FILE, path, sizeof(path));
    ensure_dir(store_dir());

    time_t now = (time_t)hal_time();
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);

    char eq[64], uq[96], dq[192];
    js_quote(evt,    eq, sizeof(eq));
    js_quote(user ? user : "-", uq, sizeof(uq));
    js_quote(detail ? detail : "", dq, sizeof(dq));

    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    fprintf(f, "{\"ts\":\"%s\",\"evt\":%s,\"user\":%s,\"res\":%d,\"detail\":%s}\n",
            ts, eq, uq, res ? 1 : 0, dq);
    fclose(f);

    /* 滚动截断：超过上限时保留最后 N 条（临时文件 + rename，避免就地读写冲突） */
    FILE *rf = fopen(path, "rb");
    if (rf) {
        int lines = 0;
        int c;
        bool in_str = false;
        while ((c = fgetc(rf)) != EOF) {
            if (c == '"') in_str = !in_str;
            if (c == '\n' && !in_str) lines++;
        }
        fclose(rf);
        if (lines > log_retention_max()) {
            int skip = lines - log_retention_max();
            char tmp_path[576];
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
            FILE *nf = fopen(path, "rb");
            FILE *tf = fopen(tmp_path, "wb");
            if (nf && tf) {
                int cur = 0, wc;
                bool ins = false;
                while ((wc = fgetc(nf)) != EOF) {
                    if (wc == '"') ins = !ins;
                    if (wc == '\n' && !ins) {
                        cur++;
                        if (cur > skip) fputc(wc, tf);   /* 保留第 skip+1 行起的换行 */
                    } else if (cur >= skip) {
                        fputc(wc, tf);                   /* 第 skip+1 行起才写内容 */
                    }
                }
            }
            if (nf) fclose(nf);
            if (tf) { fclose(tf); rename(tmp_path, path); }
            else remove(tmp_path);
        }
    }
    return 0;
}

int log_query(const char *evt_filter, int res_filter, log_entry_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    char path[560];
    file_path(LOG_FILE, path, sizeof(path));
    char *data = read_file_alloc(path);
    if (!data) return 0;

    /* 统计行数 */
    int total = 0;
    for (char *p = data; *p; p++) if (*p == '\n') total++;

    log_entry_t *arr = calloc((size_t)(total > 0 ? total : 1), sizeof(log_entry_t));
    if (!arr) { free(data); return -1; }

    int n = 0;
    char *line = strtok(data, "\n");
    while (line) {
        const char *obj = line;
        log_entry_t e;
        memset(&e, 0, sizeof(e));
        js_get_str(obj, "ts", e.ts, sizeof(e.ts));
        js_get_str(obj, "evt", e.evt, sizeof(e.evt));
        js_get_str(obj, "user", e.user, sizeof(e.user));
        long rv = 0;
        js_get_int(obj, "res", &rv);
        e.res = (int)rv;
        js_get_str(obj, "detail", e.detail, sizeof(e.detail));

        if (evt_filter && strcmp(e.evt, evt_filter) != 0) { line = strtok(NULL, "\n"); continue; }
        if (res_filter >= 0 && e.res != res_filter) { line = strtok(NULL, "\n"); continue; }
        arr[n++] = e;
        line = strtok(NULL, "\n");
    }
    free(data);

    /* 倒序：最新在前 */
    for (int i = 0; i < n / 2; i++) {
        log_entry_t t = arr[i];
        arr[i] = arr[n - 1 - i];
        arr[n - 1 - i] = t;
    }
    *out = arr;
    *count = n;
    return 0;
}

/* ==================================================================
 * 设备密钥 & 初始化
 * ================================================================== */
int devkey_get(uint8_t *key, size_t *len)
{
    static const char *dk = "LVGL-SAFE-DEVKEY-0001";   /* 固定串，后续入安全元件 */
    if (!key || !len || *len < strlen(dk)) return -1;
    memcpy(key, dk, strlen(dk));
    *len = strlen(dk);
    return 0;
}

bool store_init(void)
{
    ensure_dir(store_dir());

    /* 无 users.json：创建默认管理员（DESIGN.md §2.2 示例） */
    char path[560];
    file_path(USERS_FILE, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }

    safe_user_t boot;
    memset(&boot, 0, sizeof(boot));
    boot.id = 1;
    strncpy(boot.name, BOOT_USER_NAME, sizeof(boot.name) - 1);
    strncpy(boot.role, "admin", sizeof(boot.role) - 1);
    strncpy(boot.auth_method, "pin", sizeof(boot.auth_method) - 1);
    boot.enabled = true;
    /* 阶段 1 演示预置：管理员绑定人脸 id=1、启用 TOTP（固定演示密钥便于验收）。
     * 仅 bootstrap（首次无 users.json）生效；已存在数据时按文件字段读取。 */
    boot.face_id = 1;
    boot.face_enable = true;
    boot.totp_enable = true;
    strncpy(boot.totp_secret, "JBSWY3DPEHPK3PXP", sizeof(boot.totp_secret) - 1);
    time_t now = (time_t)hal_time();
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(boot.created_at, sizeof(boot.created_at), "%Y-%m-%dT%H:%M:%S", &tmv);
    uint8_t salt[16];
    pin_hash(BOOT_USER_PIN, salt, boot.pin_hash);
    to_hex(salt, sizeof(salt), boot.pin_salt);
    if (save_users(&boot, 1) == false) return false;

    log_append("setting_change", "system", 1, "bootstrap: default admin created");
    return true;
}

