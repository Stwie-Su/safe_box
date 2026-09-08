/**
 * @file test_store_crash.c
 * 崩溃一致性自测（技术路线规约 §10 风险项 R1 的验收项）。
 *
 * 验收两条铁律：users.json 在「写盘期间被 kill -9」之后必须
 *   1) 可解析——用存储层自己的 user_load_all() 读回来，条数与写入前一致；
 *   2) 要么全旧、要么全新——字节流严格等于「旧版参考」或「新版参考」，
 *      不允许截断 / 半截 / 新旧混合。
 *
 * 两种轮次（默认各跑 rounds 轮）：
 *   - 随机轮（RANDOM）：父进程随机 1~21ms 后 SIGKILL，覆盖写周期任意相位；
 *   - 盯梢轮（WATCH） ：父进程在子进程写盘期间连续 stat() 目标文件（忙采样，
 *                       实测 stat 仅 ~1.5µs，而本机 nanosleep(50µs) 实际要 ~1ms，
 *                       用休眠采样会把采样率降到 1/20，抓截断窗口全靠运气），
 *                       全程断言「users.json 的大小永远只能是完整大小」。
 *
 * 为什么必须有盯梢轮（这是本用例的牙齿所在）：
 *   glibc 对一次大块 fwrite 通常只发一次 write()，而 Linux 在 SIGKILL 时不会
 *   腰斩已经进入内核的 write()——所以「靠随机时刻 kill -9 撞上截断窗口」的概率
 *   极低，实测旧的非原子实现也能连过 60 轮，等于没测。盯梢轮换个角度：不赌击杀
 *   时机，而是全程盯着文件大小——非原子实现在 fopen("wb") 截断到 write() 完成
 *   之间必然露出 0 / 不完整的大小，连续上千次采样几乎必然抓到；而 tmp+rename
 *   实现下 users.json 从来不被截断，大小恒为完整值，永远抓不到中间态。
 *
 * 种子用户默认 120 个：让 users.json 到 ~50KB，保证一次写盘跨越多次系统调用，
 * 小文件几乎不存在截断窗口，测了也白测。
 *
 * 用法：test_store_crash [轮次]    默认 20 轮（每轮含 1 盯梢 + 1 随机）
 *      长跑：tools/crash_consistency_test.sh [轮次]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "core/store/store.h"
#include "hal/hal_time.h"       /* 时间源统一走 HAL，不直接 time() */

#define SEED_USERS     120     /* 种子用户数：撑大 users.json（~50KB）以制造真实写盘窗口；
                                再大只会让 load_users 的 O(n^2) 解析拖慢用例，无额外收益 */
#define ROUNDS_DEFAULT 10
#define NAME_OLD       "OLDSTATE_USER_0001"
#define NAME_NEW       "NEWSTATE_USER_0001"

#define POLL_DEADLINE_MS 100.0             /* 盯梢窗口（毫秒），到点收网 */
#define POLL_MAX         400000L           /* 盯梢采样次数上限，防止极端环境跑飞 */
#define RAND_MIN_NSEC    1000000L          /* 随机轮最短存活 1ms */
#define RAND_RNG_NSEC    20000000L         /* 随机轮存活抖动 0~20ms */

/* ---------------- 小工具 ---------------- */

/* 读整个文件到堆；失败返回 NULL。len 传出字节数。 */
static char * slurp(const char * path, size_t * len)
{
    FILE * f = fopen(path, "rb");
    if(!f) return NULL;
    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if(n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char * buf = malloc((size_t)n + 1);
    if(!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    if(len) *len = rd;
    return buf;
}

/* 整块覆盖写（仅用于铺种子和存证，不走存储层） */
static bool spit(const char * path, const char * data, size_t len)
{
    FILE * f = fopen(path, "wb");
    if(!f) return false;
    bool ok = (fwrite(data, 1, len, f) == len);
    if(fclose(f) != 0) ok = false;
    return ok;
}

static void salt_hex(const unsigned char * salt, char * out)
{
    for(int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", salt[i]);
    out[32] = '\0';
}

static void rm_rf(const char * dir)
{
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if(system(cmd) != 0) { }
}

/* ---------------- 种子：只生成一次，各轮复用字节流 ---------------- */

static char   g_seed_dir[64] = "/tmp/safe_crash_seed_XXXXXX";
static char * g_seed_data    = NULL;
static size_t g_seed_len     = 0;

static bool seed_once(void)
{
    if(!mkdtemp(g_seed_dir)) return false;
    store_set_dir(g_seed_dir);

    safe_user_t u;
    memset(&u, 0, sizeof(u));
    strcpy(u.role, "user");
    strcpy(u.auth_method, "pin");
    strcpy(u.created_at, "2026-01-01T00:00:00");   /* 固定时间戳：保证序列化字节可复现 */
    u.enabled = true;
    u.face_id = -1;
    uint8_t salt[16];
    if(pin_hash("123456", salt, u.pin_hash) != 0) return false;
    salt_hex(salt, u.pin_salt);

    for(int i = 1; i <= SEED_USERS; i++) {
        u.id = i;
        snprintf(u.name, sizeof(u.name), "user_%04d", i);
        if(user_add(&u) != 0) return false;
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/users.json", g_seed_dir);
    g_seed_data = slurp(path, &g_seed_len);
    return (g_seed_data != NULL && g_seed_len > 4096);
}

/* ---------------- 单轮：写盘 → kill -9 → 校验 ---------------- */

typedef enum { MODE_WATCH = 0, MODE_RANDOM = 1 } crash_mode_t;

static int g_rounds_watch  = 0;
static int g_rounds_random = 0;
static int g_bad_watch     = 0;
static int g_bad_random    = 0;
static long g_polls_total  = 0;
static long g_anom_total   = 0;

static bool run_round(int round_no, crash_mode_t mode)
{
    char dir[64] = "/tmp/safe_crash_round_XXXXXX";
    if(!mkdtemp(dir)) return false;

    char path[320];
    snprintf(path, sizeof(path), "%s/users.json", dir);
    if(!spit(path, g_seed_data, g_seed_len)) { rm_rf(dir); return false; }

    store_set_dir(dir);

    /* 同一条用户记录的两个版本：只有 name 不同（等长，便于字节级比对） */
    safe_user_t old_u;
    if(user_find_by_id(1, &old_u) != 0) { rm_rf(dir); return false; }
    safe_user_t new_u = old_u;
    strncpy(old_u.name, NAME_OLD, sizeof(old_u.name) - 1);
    strncpy(new_u.name, NAME_NEW, sizeof(new_u.name) - 1);

    /* 两份参考字节流：全旧 / 全新 */
    if(user_update(&old_u) != 0) { rm_rf(dir); return false; }
    size_t len_old = 0;
    char * ref_old = slurp(path, &len_old);

    if(user_update(&new_u) != 0) { free(ref_old); rm_rf(dir); return false; }
    size_t len_new = 0;
    char * ref_new = slurp(path, &len_new);

    bool ok = (ref_old != NULL && ref_new != NULL && len_old == len_new);
    long polls = 0;
    long anomalies = 0;
    double elapsed_ms = 0.0;
    off_t min_size = (off_t)len_old;

    if(ok) {
        pid_t pid = fork();
        if(pid == 0) {
            /* 子进程：死循环交替重写 users.json，直到被 SIGKILL */
            for(;;) {
                user_update(&old_u);
                user_update(&new_u);
            }
        }

        if(mode == MODE_RANDOM) {
            /* 随机相位击杀：整个写周期任意一点都可能被杀 */
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = RAND_MIN_NSEC + (long)(rand() % RAND_RNG_NSEC);
            nanosleep(&ts, NULL);
        } else {
            /* 盯梢：连续采样文件大小，抓「非完整大小」的中间态。
             * users.json 若被就地覆盖写，必然露出 0 或不完整的大小；
             * tmp+rename 下目标文件从不出现中间大小。 */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for(long i = 0; i < POLL_MAX; i++) {
                struct stat st;
                if(stat(path, &st) == 0) {
                    polls++;
                    if(st.st_size != (off_t)len_old) {
                        anomalies++;
                        if(st.st_size < min_size) min_size = st.st_size;
                        if(anomalies <= 3) {
                            printf("      !! 第 %ld 次采样抓到中间态：size=%ld（完整应为 %zu）\n",
                                   polls, (long)st.st_size, len_old);
                        }
                    }
                }
                if(anomalies >= 5) break;      /* 证据够了，提前收网 */
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double el = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                            (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
                if(el >= POLL_DEADLINE_MS) break;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                         (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
        }

        kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);

        /* ---- 终态校验：可解析 + 要么全旧要么全新 ---- */
        size_t got_len = 0;
        char * got = slurp(path, &got_len);

        bool is_old = (got != NULL && got_len == len_old && memcmp(got, ref_old, got_len) == 0);
        bool is_new = (got != NULL && got_len == len_new && memcmp(got, ref_new, got_len) == 0);

        safe_user_t * list = NULL;
        int n = 0;
        bool parse_ok = (user_load_all(&list, &n) == 0) && (n == SEED_USERS);
        bool name_ok = false;
        if(parse_ok) {
            safe_user_t first;
            if(user_find_by_id(1, &first) == 0) {
                name_ok = (strcmp(first.name, NAME_OLD) == 0) ||
                          (strcmp(first.name, NAME_NEW) == 0);
            }
        }
        user_list_free(list);

        ok = (is_old || is_new) && parse_ok && name_ok && (anomalies == 0);

        if(mode == MODE_WATCH) { g_rounds_watch++;  g_polls_total += polls; g_anom_total += anomalies; }
        else                   { g_rounds_random++; }

        printf("  轮次 %3d[%s]：%s  命中=%s  大小=%zu/%zu  解析=%d 条\n",
               round_no, mode == MODE_WATCH ? "盯梢" : "随机", ok ? "PASS" : "FAIL",
               is_old ? "全旧" : (is_new ? "全新" : "都不是"),
               got_len, len_old, n);
        if(mode == MODE_WATCH) {
            printf("           盯梢 %.0f ms / 采样 %ld 次 / 中间态 %ld 次%s%s)\n",
                   elapsed_ms, polls, anomalies,
                   anomalies ? " ← 覆盖写露出了不完整大小" : "",
                   anomalies ? "" : " ← 全程未见中间态");
        } else {
            printf("           随机时刻击杀)\n");
        }

        if(!ok && got) {
            char dump[320];                  /* 失败留证，便于人工复盘 */
            snprintf(dump, sizeof(dump), "/tmp/safe_crash_bad_%d.json", round_no);
            spit(dump, got, got_len);
            printf("           现场已存 %s\n", dump);
        }
        free(got);
    }

    free(ref_old);
    free(ref_new);
    rm_rf(dir);
    return ok;
}

int main(int argc, char ** argv)
{
    int rounds = ROUNDS_DEFAULT;
    if(argc > 1) {
        int r = atoi(argv[1]);
        if(r > 0) rounds = r;
    }

    /* 关掉 stdout 缓冲：本用例本身要造崩溃，输出必须即时落盘，
     * 否则一旦进程异常退出，缓冲区里的诊断信息会全部丢失。 */
    setbuf(stdout, NULL);

    srand((unsigned)hal_time() ^ (unsigned)getpid());

    if(!seed_once()) {
        printf("[FAIL] test_store_crash：种子数据生成失败\n");
        return 1;
    }
    printf("== 崩溃一致性自测：%d 轮盯梢 + %d 轮随机，种子 %zu 字节 / %d 用户 ==\n",
           rounds, rounds, g_seed_len, SEED_USERS);

    for(int i = 1; i <= rounds; i++) {
        if(!run_round(i, MODE_WATCH))  g_bad_watch++;
        if(!run_round(i, MODE_RANDOM)) g_bad_random++;
    }

    rm_rf(g_seed_dir);
    free(g_seed_data);

    printf("== 结果：盯梢 %d 轮失败 %d（累计采样 %ld 次 / 中间态 %ld 次）；随机 %d 轮失败 %d ==\n",
           g_rounds_watch, g_bad_watch, g_polls_total, g_anom_total,
           g_rounds_random, g_bad_random);
    if(g_bad_watch != 0 || g_bad_random != 0) {
        printf("[FAIL] test_store_crash\n");
        return 1;
    }
    printf("[PASS] test_store_crash：写盘期间任意时刻 kill -9，users.json 均完整可解析\n");
    return 0;
}
