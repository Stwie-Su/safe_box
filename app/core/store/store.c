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
/* 位置初始化：顺序与 safe_policy_t 字段一一对应，**新增字段必须在此补值**
 * （否则触发 -Wmissing-field-initializers，本项目零告警纪律）。 */
static safe_policy_t g_policy = { 4, 8, 5, 30, 3, 10, true, false };
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
                case 'u': {
                    /* QA-15：\u 后不足 4 字节（手工编辑/传输截断）时只推进到字符串结尾，
                    避免 p += 4 越界读（safe.log 半行残留里出现 \u 组合即可能越界）。 */
                    int k = 0;
                    while (k < 4 && p[1 + k]) k++;
                    p += k; out[n++] = '?'; break;
                }
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
    /* QA-23：随机盐绝不能取自未初始化的栈内存 —— 那会让 PBKDF2 的防彩虹表
     * 作用直接归零（盐可预测/重复 = 彩虹表可用）。
     * 原实现忽略 fread 返回值：/dev/urandom **打开成功但读取不足**时，
     * salt 里装的是上一次栈帧的残留数据（gcc 已报 -Wunused-result）。
     * 修法：①栈上先清零；②校验 fread 是否读满；③读不满与打不开一律走
     * rand() 兜底 —— 弱，但至少是「随机数」而不是「未知内存」。 */
    uint8_t salt[16];
    memset(salt, 0, sizeof(salt));
    FILE *f = fopen("/dev/urandom", "rb");
    bool salt_ok = false;
    if (f) {
        salt_ok = (fread(salt, 1, sizeof(salt), f) == sizeof(salt));
        fclose(f);
    }
    if (!salt_ok) {
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
    /* 不变式归一化（QA 三轮 A-#10）：face_id<0 ⟹ face_enable=false。
     * 两字段在此各自独立读盘：旧固件的清绑路径（15a1456 之前 user_face_set(uid,-1)
     * 不动 face_enable）或手工编辑会写出 face_id=-1 且 face_enable=true 的脏
     * users.json，不归一化则不一致态被带进内存并被下次落盘原样回写。 */
    if (u->face_id < 0) u->face_enable = false;
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
        bool pin_len_clamped = false;
        pthread_mutex_lock(&g_policy_mutex);
        if (js_get_int(pol, "pin_min_len", &v) && v >= 4) {
            /* H1：PIN 长度上限不得超过虚位滑窗缓冲容量（SAFE_VIRTUAL_PIN_MAX_INPUT），
             * 否则校验层 cand[] 会越界写（QA ASan 实锤）。min 也一并钳到 [4, 上限]。 */
            if (v > SAFE_VIRTUAL_PIN_MAX_INPUT) { v = SAFE_VIRTUAL_PIN_MAX_INPUT; pin_len_clamped = true; }
            g_policy.pin_min_len = (int)v;
        }
        if (js_get_int(pol, "pin_max_len", &v) && v >= 4) {
            if (v > SAFE_VIRTUAL_PIN_MAX_INPUT) { v = SAFE_VIRTUAL_PIN_MAX_INPUT; pin_len_clamped = true; }
            if (v < g_policy.pin_min_len)        { v = g_policy.pin_min_len;        pin_len_clamped = true; }
            g_policy.pin_max_len = (int)v;
        }
        if (js_get_int(pol, "max_failed", &v) && v >= 1)  g_policy.max_failed = (int)v;
        if (js_get_int(pol, "lock_seconds", &v) && v >= 1) g_policy.lock_seconds = (int)v;
        /* 阈值有合理范围才采纳；旧版 score_high/score_mid 字段直接忽略（兼容读取） */
        if (js_get_int(pol, "face_otp_after", &v) && v >= 1 && v <= 10) g_policy.face_otp_after = (int)v;
        if (js_get_int(pol, "face_verify_timeout_s", &v) && v >= 3 && v <= 120) g_policy.face_verify_timeout_s = (int)v;
        /* 虚位密码开关（FR-18）：save_users 一直有写这个字段，但此前从没在读盘时回填，
         * 导致管理员改过的开关重启即丢、内存里恒为编译期默认。此处补上读取。 */
        js_get_bool(pol, "virtual_pin_enable", &g_policy.virtual_pin_enable);
        js_get_bool(pol, "enroll_five_way",    &g_policy.enroll_five_way);
        pthread_mutex_unlock(&g_policy_mutex);
        /* 越界钳制在锁外补记审计（log_append 会在不持 g_policy_mutex 时调用）。 */
        if (pin_len_clamped)
            log_append("ALARM", "-", 0, "策略 PIN 长度越界，已钳制到合法范围");
    }

    const char *arr = js_get(json, "users");
    if (!arr) { free(json); if (ok) *ok = false; return 0; }
    int n = js_arr_count(arr);
    if (n <= 0) { free(json); return 0; }

    safe_user_t *us = calloc((size_t)n, sizeof(safe_user_t));
    if (!us) { free(json); if (ok) *ok = false; return 0; }
    /* QA-27：解析失败的槽位不再保留零值用户（id=0/空 name 的幽灵用户会被后续
       save_users 原样写回，污染用户表并干扰 user_del/find_by_id/next_id）。
       紧凑数组：只保留成功解析的用户，count 同步收窄。 */
    int valid = 0;
    for (int i = 0; i < n; i++) {
        const char *uobj = js_arr_at(arr, i);
        if (!uobj || !parse_user_obj(uobj, &us[valid])) { if (ok) *ok = false; continue; }
        valid++;
    }
    free(json);
    *list = us;
    *count = valid;
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
    /* QA-20：必须走 user_policy() 取**加锁快照**，不能直接取 &g_policy ——
     * 本函数在 worker 线程写盘，而主线程可能同时在改策略，直接取地址会让
     * 下面 7 次字段读取与写入并发（TSan 实锤的竞争点之一）。 */
    const safe_policy_t *p = user_policy();
    int w;
    /* QA-13：snprintf 返回「本应写入的长度」，截断时 o 会越过 cap，
       后续 cap - o 因 size_t 无符号下溢成巨大值 → 越界写堆。
       这里每次都校验返回值，越界即放弃，绝不累积下溢。 */
    w = snprintf(buf + o, cap - o,
        "{\n  \"version\": 1,\n  \"policy\": {\"pin_min_len\": %d, \"pin_max_len\": %d, "
        "\"max_failed\": %d, \"lock_seconds\": %d, \"face_otp_after\": %d, "
        "\"face_verify_timeout_s\": %d, \"virtual_pin_enable\": %s, \"enroll_five_way\": %s},\n  \"users\": [\n",
        p->pin_min_len, p->pin_max_len, p->max_failed, p->lock_seconds,
        p->face_otp_after, p->face_verify_timeout_s, p->virtual_pin_enable ? "true" : "false",
        p->enroll_five_way ? "true" : "false");
    if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
    o += (size_t)w;
    for (int i = 0; i < n; i++) {
        char one[1024];   /* 放大到足以容纳任意单用户 JSON，避免 user_to_json 自身截断 */
        user_to_json(&us[i], one, sizeof(one));
        w = snprintf(buf + o, cap - o, "    %s%s\n", one, (i < n - 1) ? "," : "");
        if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
        o += (size_t)w;
    }
    w = snprintf(buf + o, cap - o, "  ]\n}\n");
    if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
    o += (size_t)w;
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
    /* QA-16：显式 uid 唯一性校验（远程 add_user 可直接指定 id）。重复 id 一旦写入，
       user_update / user_face_set / user_find_by_id 都只命中第一个，导致后续对该 id
       的更新静默改错人。统一在 user_add 把关（防御纵深，不依赖调用方）。 */
    if (u->id > 0) {
        for (int i = 0; i < n; i++) {
            if (us[i].id == u->id) { user_list_free(us); return -3; }
        }
    }
    safe_user_t *nu = realloc(us, (size_t)(n + 1) * sizeof(safe_user_t));
    if (!nu) { user_list_free(us); return -1; }
    nu[n] = *u;
    /* 不变式（QA 三轮 A-#10）：调用方传入的结构体同样强制归一化——
     * 任何写路径都不得把 face_id<0 且 face_enable=true 的不一致态写进存储。 */
    if (nu[n].face_id < 0) nu[n].face_enable = false;
    bool ok = save_users(nu, n + 1);
    user_list_free(nu);
    return ok ? 0 : -1;
}

int user_del_cascade(int id, int32_t *out_face_id)
{
    if (out_face_id) *out_face_id = -1;
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
            /* QA-02：先拷名字再释放 —— 原实现 free 之后仍读 us[target_idx].name
             * （use-after-free，ASan 实锤，删除唯一启用管理员时必现）。 */
            char del_name[32];
            strncpy(del_name, us[target_idx].name, sizeof(del_name) - 1);
            del_name[sizeof(del_name) - 1] = '\0';
            user_list_free(us);
            log_append("user_del", del_name, 0, "拒绝：需保留至少一个启用管理员");
            return -1;
        }
    }

    /* ★ uid 归属核验（防误删）：只有当本地表里「该 face_id 确实绑定在目标用户」
     * 时才把模板号交出去删模组。
     * 背景：模组侧 uid 与应用侧 user_id 是两套编号，靠 users.json 的 face_id 映射。
     * 若映射错乱（手工编辑、对账滞后、模组回收复用 uid 等），贸然下发 DELETE
     * 会删掉模组里**另一个用户**的脸 —— 这是不可逆且难以察觉的事故。
     * 取舍：映射异常时本地用户照删（业务意图要执行），但绝不删模组模板，
     *       宁可留孤儿（由 cred_reconcile 启动对账标失效）。 */
    int32_t fid_del = -1;
    if (us[target_idx].face_id >= 0) {
        safe_user_t owner;
        memset(&owner, 0, sizeof(owner));
        if (user_find_by_face(us[target_idx].face_id, &owner) == 0 &&
            owner.id == id) {
            fid_del = us[target_idx].face_id;      /* 归属确认 -> 允许删模组 */
        } else {
            /* 映射异常：记录告警（在 free 之前取名字，避免 UAF） */
            log_append("ALARM", us[target_idx].name, 0,
                       "人脸模板映射异常，未删除模组模板（防误删）");
        }
    }

    int keep = 0;
    for (int i = 0; i < n; i++) {
        if (us[i].id != id) us[keep++] = us[i];
    }
    bool ok = save_users(us, keep);
    user_list_free(us);
    /* 仅当本地确实删成功、且归属核验通过时，才把模板号交出去 */
    if (ok && out_face_id && fid_del >= 0) *out_face_id = fid_del;
    return ok ? 0 : -1;
}

int user_del(int id)
{
    return user_del_cascade(id, NULL);
}

/* 清空**所有用户**的人脸绑定（face_id=-1 且 face_enable=false）。
 * 调用时机：模组侧被整体清空（DELETE_ALL）之后，本地必须同步清掉 —— 否则会留下
 * 一整批「本地有绑定、模组无模板」的孤儿，用户刷脸必失败且自己删不掉。
 * 保持不变式：face_id < 0 ⟹ face_enable == false。
 * 无改动时**不写盘**（避免无谓的 IO 与原子重写）。 */
int user_face_clear_all(void)
{
    safe_user_t *us = NULL;
    int n = 0;
    if (load_users(&us, &n, NULL) != 0 || n <= 0) { user_list_free(us); return -1; }

    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (us[i].face_id >= 0 || us[i].face_enable) {
            us[i].face_id     = -1;
            us[i].face_enable = false;
            changed = true;
        }
    }
    bool ok = changed ? save_users(us, n) : true;
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
 *
 * 语义（face_enable = 「该用户的人脸通道当前是否可用」的可用性状态，非「管理员意愿」）：
 *   不变式：face_enable == true ⟹ face_id >= 0（有可用通道必有绑定；逆否即
 *   face_id < 0 ⟹ face_enable == false）。本函数内 face_id 与本用户 face_enable
 *   同步维护，不破坏该不变式：
 *     face_id >= 0（录入/重绑） -> face_enable = true（录入成功即自动启用）；
 *     face_id < 0 （清除绑定） -> face_enable = false（无绑定 -> 通道不可用）。
 *   「管理员是否允许此人用脸」的意愿由「允许其录脸」表达，而录入成功即自动启用，
 *   故清绑时置 false 不丢失任何管理信息。
 * face_id >= 0 时顺带把其它用户身上相同的模板号清成 -1 并停用其通道：FM225 一个
 * 模板只属于一个用户，防御性去重保证 user_find_by_face 的映射唯一，且同样守不变式。 */
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
            if (i != idx && us[i].face_id == face_id) {
                /* 模板号被本用户抢走：该用户不再有绑定 -> 通道一并停用，维持不变式
                 * 「face_enable == true ⟹ face_id >= 0」。否则会出现 face_enable=true
                 * 而 face_id=-1 的不一致态：该用户刷脸永不命中（user_find_by_face 要求
                 * face_id>=0）-> 被判「未注册人脸」记一次失败，界面也会误显示已启用。 */
                us[i].face_id     = -1;
                us[i].face_enable = false;
            }
        }
    }
    /* face_id 与本人 face_enable 同步维护（守不变式，见函数头）：
     *   >= 0（录入/重绑）-> true：这是「凭据恢复」路径 —— face_enable 会被三条通道置
     *     false（auth_fsm 名字核对不符 FR-21 防线1、启动对账标孤儿 FR-21 防线3、管理员
     *     关闭）；若重绑只写 face_id 不置 true，用户重新录入人脸后 resolve() 仍会因
     *     face_enable==false 永久拒绝（QA 复核 高危#2：真机复录后无法人脸开锁）。
     *   < 0（清除绑定）-> false：无绑定则通道不可用（QA 二轮：清了 face_id 却留
     *     face_enable=true 会造出不一致态，故此处必须一并置 false）。 */
    us[idx].face_id     = face_id;
    us[idx].face_enable = (face_id >= 0);

    bool ok = save_users(us, n);
    user_list_free(us);
    return ok ? 0 : -1;
}

const safe_policy_t * user_policy(void)
{
    /* ★ 线程安全（QA-20）：快照放**线程局部存储**。
     *
     * 原实现是函数内 `static safe_policy_t snap` —— 它只解决了「调用方
     * 直接持有 &g_policy」，却引入了自己的竞争：
     *   ① 两个线程同时调用会互相覆盖同一份 snap，先返回者拿到的内容已被改写；
     *   ② 调用方在锁外读 snap 的字段时，另一线程可能正在锁内写 snap。
     * 本函数有 14 处调用点（UI 层为主），而 g_policy 会被 worker 线程改
     * （user_policy_set* / load_users 回写），所以这是真实可达的竞争。
     *
     * 用 __thread（GCC/Clang 在 ARM 与 x86 均支持）而非「改成传 buffer 的签名」，
     * 是为了不动那 14 处调用点、把风险压到最小。每线程一份 32 字节，代价可忽略。
     * 注：g_policy 自身的读写仍由 g_policy_mutex 保护（见 user_policy_set*）。 */
    static __thread safe_policy_t snap;
    pthread_mutex_lock(&g_policy_mutex);
    snap = g_policy;
    pthread_mutex_unlock(&g_policy_mutex);
    return &snap;
}

/* ★ 策略落盘的顺序不变式（2026-09-16 修复）：
 * load_users() 会把 users.json 里的 policy 读出来同步回 g_policy。因此三个
 * user_policy_set* 必须严格按「先 load（同步基线）→ 改内存 → 最后 save」执行。
 * 修复前 user_policy_set / user_policy_set_face 是「先改内存 → 再 load」：
 * load 的回写把刚改的 max_failed / lock_seconds / 人脸阈值原样冲掉，
 * 于是 SYSTEM 页「保存策略」改的值重启即丢（磁盘上仍是旧值）。
 * 注意 save_users() 写的是全局 g_policy，所以这三步之间不能插入其它策略写入。 */
void user_policy_set(int max_failed, int lock_seconds)
{
    /* 落盘：先读出全部用户（同时把文件里的 policy 同步进 g_policy），再改内存 */
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);

    pthread_mutex_lock(&g_policy_mutex);
    if (max_failed >= 1 && max_failed <= 10) g_policy.max_failed = max_failed;
    if (lock_seconds >= 5 && lock_seconds <= 3600) g_policy.lock_seconds = lock_seconds;
    pthread_mutex_unlock(&g_policy_mutex);

    /* QA-24：users.json 缺失/损坏导致 n==0 时，策略修改（阈值/虚位开关）也必须落盘，
       否则 SYSTEM 页改的值重启即丢。save_users(NULL,0) 产出合法空数组 JSON。 */
    if (n > 0) save_users(us, n);
    else       save_users(NULL, 0);
    user_list_free(us);
}

void user_policy_set_face(int otp_after, int timeout_s)
{
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);

    pthread_mutex_lock(&g_policy_mutex);
    if (otp_after >= 1 && otp_after <= 10)      g_policy.face_otp_after = otp_after;
    if (timeout_s >= 3  && timeout_s <= 120)    g_policy.face_verify_timeout_s = timeout_s;
    pthread_mutex_unlock(&g_policy_mutex);

    /* QA-24：users.json 缺失/损坏导致 n==0 时，策略修改（阈值/虚位开关）也必须落盘，
       否则 SYSTEM 页改的值重启即丢。save_users(NULL,0) 产出合法空数组 JSON。 */
    if (n > 0) save_users(us, n);
    else       save_users(NULL, 0);
    user_list_free(us);
}

/* 虚位密码开关（FR-18）落盘 —— 与上面两个 setter 同一顺序不变式。 */
/* 录入模式（单帧/五向）落盘 —— 与其它 setter 同一顺序不变式：
 * 先 load（同步基线）→ 改内存 → 最后 save。 */
void user_policy_set_enroll_mode(bool five_way)
{
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);
    pthread_mutex_lock(&g_policy_mutex);
    g_policy.enroll_five_way = five_way;
    pthread_mutex_unlock(&g_policy_mutex);

    if (n > 0) save_users(us, n);
    else       save_users(NULL, 0);
    user_list_free(us);
}

void user_policy_set_virtual_pin(bool enable)
{
    safe_user_t *us = NULL;
    int n = 0;
    load_users(&us, &n, NULL);
    pthread_mutex_lock(&g_policy_mutex);
    g_policy.virtual_pin_enable = enable;
    pthread_mutex_unlock(&g_policy_mutex);
    /* QA-24：users.json 缺失/损坏导致 n==0 时，策略修改（阈值/虚位开关）也必须落盘，
       否则 SYSTEM 页改的值重启即丢。save_users(NULL,0) 产出合法空数组 JSON。 */
    if (n > 0) save_users(us, n);
    else       save_users(NULL, 0);
    user_list_free(us);
}

/* 角色合法性（FR-1 角色表 / FR-9 临时授权）：空指针与未知角色一律判非法，
 * 供创建入口（UI 添加用户 / RPC add_user）在创建时把关，避免把脏 role 写进用户表。 */
bool user_role_valid(const char * role)
{
    if (role == NULL || role[0] == '\0') return false;
    return strcmp(role, "admin") == 0
        || strcmp(role, "user")  == 0
        || strcmp(role, "temp")  == 0;
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
    /* QA-20：策略必须走 user_policy() 取**加锁快照**（其快照已改为 __thread）。
     * 本函数在 **worker 线程**执行（PIN 校验已异步化），直接读 g_policy 会与
     * 主线程的 user_policy_set* / load_users 回写并发 —— TSan 实锤的竞争点。 */
    const safe_policy_t *pol = user_policy();
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
            if (us[i].failed_attempts >= pol->max_failed) {
                us[i].lock_until = now + pol->lock_seconds;
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
    /* QA-13：单条预留从 640 放大到 768（ssid160+sec64+psk_enc400+json 开销），
       避免长 psk_enc 时整条 JSON 被 snprintf 截断；下方逐段校验返回值防 size_t 下溢。 */
    size_t cap = 256 + (size_t)n * 768;
    char *buf = malloc(cap);
    if (!buf) return false;
    size_t o = 0;
    int w;
    w = snprintf(buf + o, cap - o, "{\n  \"version\": 1,\n  \"networks\": [\n");
    if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
    o += (size_t)w;
    for (int i = 0; i < n; i++) {
        char sq[160], seq[64];
        js_quote(ns[i].ssid, sq, sizeof(sq));
        js_quote(ns[i].sec,  seq, sizeof(seq));
        w = snprintf(buf + o, cap - o,
            "    {\"ssid\":%s,\"sec\":%s,\"psk_enc\":\"%s\"}%s\n",
            sq, seq, ns[i].psk_enc, (i < n - 1) ? "," : "");
        if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
        o += (size_t)w;
    }
    w = snprintf(buf + o, cap - o, "  ]\n}\n");
    if (w < 0 || (size_t)w >= cap - o) { free(buf); return false; }
    o += (size_t)w;
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
    /* QA-11：追加后必须 fsync。否则掉电时最后一条可能只写了一半 ——
     * 留下的「无换行结尾的残缺行」正是 QA-01（log_query 越界写）的触发源，
     * 这里 fsync 是从源头减少半行。 */
    fflush(f);
    fsync(fileno(f));
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
            /* QA-22（严重）：原实现在 **nf 打开失败而 tf 成功** 时仍会 rename ——
             * 用**空的临时文件**覆盖整份 safe.log，等于把全部审计日志抹掉。
             * 修法：只有「源与临时文件都打开成功」且「写入无错」时才 rename；
             *       其余一律删除临时文件、保留原文件 —— 宁可日志超上限，
             *       也绝不能把审计链整条弄丢。 */
            bool rotate_ok = false;
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
                rotate_ok = !ferror(tf);
                if (rotate_ok) {
                    fflush(tf);
                    fsync(fileno(tf));      /* QA-11：临时文件先落盘再 rename */
                }
            }
            if (nf) fclose(nf);
            if (tf) fclose(tf);
            if (rotate_ok) {
                if (rename(tmp_path, path) == 0) fsync_parent_dir(path);
                else remove(tmp_path);
            } else {
                remove(tmp_path);
            }
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

    /* QA-01（语义面）：先丢弃末尾「无换行结尾」的残缺半行。
     * 掉电 / kill -9 时 log_append 可能只写下半条记录，它不是一条有效日志；
     * 若放任它参与解析，日志页会凭空多出一条四个字段全空的条目。
     * 注意：仅截断末尾那一段，其前的完整行必须保留。 */
    size_t dlen = strlen(data);
    if (dlen > 0 && data[dlen - 1] != '\n') {
        char *last_nl = strrchr(data, '\n');
        if (last_nl) *last_nl = '\0';   /* 截掉半行，保留其前的完整行 */
        else         data[0]  = '\0';   /* 整份文件仅一行且不完整 -> 视为空 */
    }

    /* 统计行数 */
    int total = 0;
    for (char *p = data; *p; p++) if (*p == '\n') total++;

    /* QA-01：strtok 切出的「段数」= 完整行数 + 末尾半行（无 '\n' 结尾的残缺记录）。
     * 掉电 / kill -9 时 log_append 可能只写下半行，段数比 '\n' 个数多 1，
     * 按 total 分配会越界写 sizeof(log_entry_t) 字节。故容量取 total + 1。 */
    size_t cap = (size_t)total + 1;
    log_entry_t *arr = calloc(cap, sizeof(log_entry_t));
    if (!arr) { free(data); return -1; }

    int n = 0;
    /* QA-12：用 strtok_r 而非 strtok —— 本函数会在 **worker 线程** 被调用
     * （经 astore_query_log），而 strtok 跨调用持有静态状态，与其它线程的
     * strtok 调用会互相破坏（非线程安全）。 */
    char *saveptr = NULL;
    char *line = strtok_r(data, "\n", &saveptr);
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

        if (evt_filter && strcmp(e.evt, evt_filter) != 0) { line = strtok_r(NULL, "\n", &saveptr); continue; }
        if (res_filter >= 0 && e.res != res_filter) { line = strtok_r(NULL, "\n", &saveptr); continue; }
        if (n >= (int)cap) break;      /* QA-01：防御性守卫，绝不越界写 */
        arr[n++] = e;
        line = strtok_r(NULL, "\n", &saveptr);
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
    /* QA-26：出厂预置管理员**不得**预填 face_id 与 face_enable。
     * 原实现写 face_id=1 / face_enable=true，但设备上根本没有 id=1 的模组模板
     * （FM225 未接或从未录入）—— 于是界面显示一个并不存在的人脸绑定：
     *   ① 违反不变式「face_enable == true ⟹ face_id >= 0 且模板真实存在」；
     *   ② 用户刷脸必失败（user_find_by_face 命中不到真模板）；
     *   ③ 启动对账会把它判成孤儿凭据，产生一条误导性的告警。
     * 正确做法：预置时无绑定（face_id=-1 / face_enable=false），
     * 由用户后续走正常录入流程绑定 —— 那时 face_id 才是模组真实分配的。 */
    boot.face_id = -1;
    boot.face_enable = false;
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

