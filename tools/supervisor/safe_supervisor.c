/**
 * @file safe_supervisor.c
 * 应用守护进程：崩溃重启 + 限速 + 安全态 + 看门狗接管。
 *
 * 【职责】
 *   1) fork + exec 应用，waitpid 监控退出状态；
 *   2) 区分「正常退出」与「异常崩溃」——正常退出 / 被 SIGTERM、SIGINT 停止 → 不重启；
 *      非 0 退出码，或被 SIGSEGV、SIGABRT 等杀死 → 重启；
 *   3) 重启限速：滑动时间窗内重启次数超过阈值 → 判定设备异常 → 进入安全态（不再重启）；
 *   4) 指数退避：连续崩溃时间隔逐次翻倍（上限 --restart-delay 的 8 倍），
 *      避免「崩溃 → 立刻拉起 → 再崩溃」把 CPU 与日志打满；
 *   5) 安全态（fail-safe）：先执行安全动作钩子（默认 /etc/safe/safe_state.sh，
 *      真锁接入后指向「拉低 GPIO、保持锁闭」），再**接管硬件看门狗**持续喂狗。
 *      ★ 为什么必须接管：应用崩溃后没人喂狗，15s 后整机复位 → init 又拉起
 *        supervisor → 又崩溃，形成「整机反复重启」。接管后设备保持运行且锁闭，
 *        日志与状态文件可查，等人来修 —— 这才是安全设备的失效方向。
 *   6) 优雅停机：supervisor 收到 SIGTERM/SIGINT → 转发 SIGTERM 给应用，
 *      宽限 grace 秒仍未退出则 SIGKILL；
 *   7) 单实例：flock 锁文件，重复启动直接退出（退出码 3），避免两个 supervisor
 *      抢同一个应用与看门狗；
 *   8) 状态可观测：--status 指定 key=value 状态文件（原子替换），运维与测试都读它。
 *
 * 【为什么是独立进程而不是应用内自重启】
 *   应用崩溃时其自身状态已不可信（堆损坏 / 死锁 / 死锁在库里），自救不可靠；
 *   父进程只依赖内核的 fork/waitpid 语义，天然能在子进程任何死法下拿到通知。
 *
 * 编译（与 tools/ 下的 cam_probe.c、io_bench.c 同一惯例：手工编译，不进 CMake 主构建）：
 *   cc -O2 -Wall -Wextra -o safe_supervisor safe_supervisor.c
 *
 * 用法：
 *   ./safe_supervisor --app /usr/share/lvgl/lvglsim [选项] [-- 应用参数...]
 * 选项：
 *   --app PATH           被守护的应用（必填，且必须可执行）
 *   --max-restarts N     滑动窗口内允许的重启次数（默认 5）
 *   --window SEC         滑动窗口长度，秒（默认 60）
 *   --grace SEC          优雅停机宽限，秒（默认 5）
 *   --restart-delay MS   首次重启间隔，毫秒（默认 500；连续崩溃指数退避）
 *   --safe-cmd CMD       进入安全态时执行的命令（默认 /etc/safe/safe_state.sh）
 *   --watchdog DEV       安全态接管的看门狗设备（默认 /dev/watchdog；给 none 不接管）
 *   --log FILE           日志落盘（默认 stderr）
 *   --log-max BYTES      单个日志文件上限，超过轮转为 .1（默认 1048576；0=不轮转）
 *   --app-log FILE       应用 stdout/stderr 追加到该文件（默认继承 supervisor）
 *   --heartbeat FILE     软狗心跳文件（应用侧 SAFE_WATCHDOG_BACKEND=file 写同一路径）
 *   --heartbeat-timeout SEC  心跳超过该秒数未更新 → 判定卡死并重启（默认 5）
 *   --heartbeat-startup SEC  启动宽限：应用初始化期间不判死（默认 15）
 *   --status FILE        状态文件路径（默认不写）
 *   --pidfile FILE       单实例锁 + PID 文件（默认 /var/run/safe_supervisor.pid）
 *   --daemon             转入后台（setsid + 二次 fork + stdio 重定向）
 *   --help               显示本帮助
 *
 * 退出码：0=应用正常退出（未重启）；1=参数或启动错误；2=进入安全态；3=已有实例在跑。
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/watchdog.h>

#define DEFAULT_MAX_RESTARTS      5
#define DEFAULT_WINDOW_SEC        60
#define DEFAULT_GRACE_SEC         5
#define DEFAULT_RESTART_DELAY_MS  500
#define DEFAULT_SAFE_CMD          "/etc/safe/safe_state.sh"
#define DEFAULT_WATCHDOG_DEV      "/dev/watchdog"
#define DEFAULT_LOG_MAX           (1024 * 1024)
#define DEFAULT_PIDFILE           "/var/run/safe_supervisor.pid"
#define DEFAULT_HB_TIMEOUT_SEC    5
#define DEFAULT_HB_STARTUP_SEC    15
#define MAX_TRACKED               64
#define POLL_MS                   100
#define BACKOFF_MAX_FACTOR        8

static volatile sig_atomic_t g_stop = 0;
static volatile pid_t        g_child = -1;
static FILE *                g_log = NULL;
static const char *          g_log_path = NULL;
static long                  g_log_max = DEFAULT_LOG_MAX;
static const char *          g_status_path = NULL;
static const char *          g_hb_path = NULL;      /* 软狗心跳文件；NULL = 不启用 */
static int                   g_hb_timeout_s = DEFAULT_HB_TIMEOUT_SEC;
static int                   g_hb_startup_s = DEFAULT_HB_STARTUP_SEC;
static int                   g_lock_fd = -1;

static time_t g_restarts[MAX_TRACKED];
static int    g_nrestart = 0;

/* 毫秒睡眠：用 nanosleep 而不是 usleep —— 后者在 _POSIX_C_SOURCE=200809L
 * 下不可见（POSIX.1-2001 已把它移出标准），交叉工具链会直接报隐式声明。 */
static void sleep_ms(int ms)
{
    if(ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 单调时钟（毫秒）：算「启动后过了多久」，不受系统时间调整影响。 */
static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* 墙上时钟（毫秒）：与文件 mtime 同源，算「心跳多久没更新」。 */
static long long wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* 心跳文件里是否已经写了 stopped（应用正常退出时留下）。
 * 正常退出不该被当成卡死再拉起来。读失败按「没有标记」处理。 */
static int hb_is_stopped(void)
{
    if(g_hb_path == NULL) return 0;
    int fd = open(g_hb_path, O_RDONLY);
    if(fd < 0) return 0;

    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return (n >= 7 && strncmp(buf, "stopped", 7) == 0) ? 1 : 0;
}

/* 软狗判定：
 *   1  = 心跳过期（应用卡死）
 *   0  = 正常
 *  -1  = 文件还没出现，但仍在启动宽限内（应用初始化中）
 * age_ms 回填「距上次心跳/启动的毫秒数」，便于日志说明。 */
static int hb_check(long long child_started_ms, long long * age_ms)
{
    if(g_hb_path == NULL) return 0;

    struct stat st;
    if(stat(g_hb_path, &st) == 0) {
        long long m = (long long)st.st_mtim.tv_sec * 1000LL + st.st_mtim.tv_nsec / 1000000L;
        long long age = wall_ms() - m;
        if(age_ms) *age_ms = age;
        if(age > (long long)g_hb_timeout_s * 1000LL) {
            if(hb_is_stopped()) return 0;
            return 1;
        }
        return 0;
    }

    long long age = monotonic_ms() - child_started_ms;
    if(age_ms) *age_ms = age;
    return (age > (long long)g_hb_startup_s * 1000LL) ? 1 : -1;
}

/* ------------------------------------------------------------------
 * 函数：log_rotate
 * 功能：日志超过 --log-max 时把当前文件改名为 .1（只保留一份历史）。
 * 说明：嵌入式 flash 容量小，日志必须自限；轮转失败不阻断主流程。
 * ------------------------------------------------------------------ */
static void log_rotate(void)
{
    if(g_log == NULL || g_log_path == NULL || g_log_max <= 0) return;

    long sz = ftell(g_log);
    if(sz < 0 || sz < g_log_max) return;

    fclose(g_log);
    g_log = NULL;

    char old[512];
    snprintf(old, sizeof(old), "%s.1", g_log_path);
    (void)rename(g_log_path, old);

    g_log = fopen(g_log_path, "a");
    if(g_log == NULL) {
        /* 轮转后打不开：退回 stderr（daemon 模式下会丢，但 --log 不可写属配置错误）。 */
        g_log = NULL;
    }
}

/* ------------------------------------------------------------------
 * 函数：log_line
 * 功能：带时间戳的日志（stderr 或 --log 指定文件）。
 * ------------------------------------------------------------------ */
static void log_line(const char * fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    if(g_log != NULL) log_rotate();
    FILE * out = (g_log != NULL) ? g_log : stderr;

    va_list ap;
    fprintf(out, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

/* ------------------------------------------------------------------
 * 函数：status_write
 * 功能：原子写状态文件（tmp + rename），供运维脚本与测试读取。
 *   格式：pid=... / state=running|safe|stopped / restarts=N /
 *         since=epoch / app=... / last_exit=...
 * ------------------------------------------------------------------ */
static void status_write(const char * state, const char * last_exit)
{
    if(g_status_path == NULL || *g_status_path == '\0') return;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_path);

    FILE * f = fopen(tmp, "w");
    if(f == NULL) return;
    fprintf(f, "pid=%d\n", (int)getpid());
    fprintf(f, "state=%s\n", state ? state : "unknown");
    fprintf(f, "restarts=%d\n", g_nrestart);
    fprintf(f, "since=%ld\n", (long)time(NULL));
    fprintf(f, "last_exit=%s\n", last_exit ? last_exit : "-");
    fflush(f);
    if(fsync(fileno(f)) != 0) { /* 落盘失败不阻断：状态文件只是可观测性 */ }
    fclose(f);
    (void)rename(tmp, g_status_path);
}

/* ------------------------------------------------------------------
 * 函数：on_signal（信号处理）
 * 功能：只置标志 + 向子进程转发 SIGTERM。绝不在信号上下文做 IO/分配（规约 §3.2 同款纪律）。
 * ------------------------------------------------------------------ */
static void on_signal(int sig)
{
    g_stop = 1;
    if(g_child > 0) kill(g_child, SIGTERM);
    (void)sig;
}

/* 滑动窗口内的重启次数 */
static int count_in_window(time_t now, int window)
{
    int n = (g_nrestart < MAX_TRACKED) ? g_nrestart : MAX_TRACKED;
    int c = 0;
    for(int i = 0; i < n; i++) {
        int idx = (g_nrestart - 1 - i) % MAX_TRACKED;
        if(idx < 0) idx += MAX_TRACKED;
        if(now - g_restarts[idx] <= (time_t)window) c++;
    }
    return c;
}

static void record_restart(time_t now)
{
    g_restarts[g_nrestart % MAX_TRACKED] = now;
    g_nrestart++;
}

/* 把信号名落成可读文本（不依赖 strsignal 的本地化，日志要稳定可 grep）。 */
static const char * signal_name(int sig)
{
    switch(sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGTERM: return "SIGTERM";
        case SIGINT:  return "SIGINT";
        case SIGPIPE: return "SIGPIPE";
        default:      return "SIG?";
    }
}

/* ------------------------------------------------------------------
 * 函数：watchdog_takeover
 * 功能：安全态打开看门狗并设定超时，返回 fd（失败 -1）。
 * 说明：应用已退出，fd 已释放；这里重新 open 通常能成功。
 *       若驱动是 nowayout 或仍被占用，只能记录 —— 此时设备最终会被复位，
 *       这是「无狗可接管」时唯一可接受的最后手段。
 * ------------------------------------------------------------------ */
static int watchdog_takeover(const char * dev, int timeout_s)
{
    if(dev == NULL || strcmp(dev, "none") == 0) return -1;

    int fd = open(dev, O_WRONLY);
    if(fd < 0) {
        log_line("安全态无法接管看门狗 %s: %s（超时后设备可能复位）", dev, strerror(errno));
        return -1;
    }

    int sec = (timeout_s > 0) ? timeout_s : 15;
    if(ioctl(fd, WDIOC_SETTIMEOUT, &sec) < 0) {
        log_line("安全态设定看门狗超时失败: %s（沿用驱动默认）", strerror(errno));
    }
    if(ioctl(fd, WDIOC_KEEPALIVE, 0) < 0) {
        if(write(fd, "\0", 1) != 1) {
            log_line("安全态首次喂狗失败: %s", strerror(errno));
        }
    }
    log_line("安全态已接管看门狗 %s（超时 %ds）", dev, sec);
    return fd;
}

/* 安全态：持续喂狗直到收到停机信号；退出前 magic close。 */
static void watchdog_hold(int fd)
{
    if(fd < 0) return;
    while(!g_stop) {
        sleep(1);
        if(g_stop) break;
        if(ioctl(fd, WDIOC_KEEPALIVE, 0) < 0) {
            if(write(fd, "\0", 1) != 1) {
                log_line("安全态喂狗失败: %s", strerror(errno));
            }
        }
    }
    if(write(fd, "V", 1) != 1) {
        log_line("安全态 magic close 失败: %s（关闭后仍可能被复位）", strerror(errno));
    }
    close(fd);
    log_line("安全态看门狗已停止（magic close）");
}

/* ------------------------------------------------------------------
 * 函数：enter_safe_state
 * 功能：进入安全态——执行安全动作钩子、接管看门狗并挂起等待人工介入。
 * 说明：钩子不存在/不可执行时只记录，绝不静默吞掉
 *       （安全设备的硬要求：失效必须导向安全侧且有痕）。
 * ------------------------------------------------------------------ */
static void enter_safe_state(const char * safe_cmd, const char * wd_dev, const char * why)
{
    log_line("SAFE-STATE: %s（停止重启，设备保持安全态）", why);
    status_write("safe", "safe-state");

    /* 先接管狗再执行钩子：钩子若卡住，至少不会在 15s 后被整机复位。 */
    int wd = watchdog_takeover(wd_dev, 15);

    if(safe_cmd != NULL && *safe_cmd != '\0') {
        if(access(safe_cmd, X_OK) == 0) {
            log_line("执行安全动作: %s", safe_cmd);
            int rc = system(safe_cmd);
            log_line("安全动作返回 rc=%d", rc);
        }
        else {
            log_line("安全动作不可用（不存在或不可执行）: %s —— 仅记录，不重启", safe_cmd);
        }
    }

    watchdog_hold(wd);
}

/* ------------------------------------------------------------------
 * 函数：daemonize
 * 功能：标准双 fork 转后台（setsid → fork → chdir / → umask 0 → stdio 指向 /dev/null）。
 * 返回值：0 = 当前进程已是守护进程；-1 = 失败。
 * ------------------------------------------------------------------ */
static int daemonize(void)
{
    pid_t pid = fork();
    if(pid < 0) return -1;
    if(pid > 0) _exit(0);

    if(setsid() < 0) return -1;

    pid = fork();
    if(pid < 0) return -1;
    if(pid > 0) _exit(0);

    if(chdir("/") != 0) { /* 失败不致命：只是工作目录没切走 */ }
    umask(0);

    int fd = open("/dev/null", O_RDWR);
    if(fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if(fd > STDERR_FILENO) close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * 函数：single_instance
 * 功能：flock 排他锁 + 写入 PID；返回保持打开的 fd（进程存活期间一直持有）。
 * 返回值：>=0 = 锁 fd；-1 = 无法打开；-2 = 已有实例。
 * ------------------------------------------------------------------ */
static int single_instance(const char * path)
{
    if(path == NULL || *path == '\0') return -1;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if(fd < 0) return -1;
    if(flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -2;
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if(ftruncate(fd, 0) != 0) { /* 截断失败不阻断：下面写入会覆盖 */ }
    if(write(fd, buf, (size_t)n) != n) { /* 写失败不阻断：锁本身已生效 */ }
    return fd;
}

static void usage(const char * self)
{
    fprintf(stderr,
            "用法: %s --app <path> [选项] [-- 应用参数...]\n"
            "  --app PATH           被守护的应用（必填）\n"
            "  --max-restarts N     滑动窗口内允许的重启次数（默认 %d）\n"
            "  --window SEC         滑动窗口长度（默认 %d）\n"
            "  --grace SEC          优雅停机宽限（默认 %d）\n"
            "  --restart-delay MS   首次重启间隔（默认 %d，连续崩溃指数退避）\n"
            "  --safe-cmd CMD       安全态动作（默认 %s）\n"
            "  --watchdog DEV       安全态接管的看门狗（默认 %s；none=不接管）\n"
            "  --log FILE           日志落盘（默认 stderr）\n"
            "  --log-max BYTES      日志文件上限（默认 %d；0=不轮转）\n"
            "  --app-log FILE       应用 stdout/stderr 追加到该文件\n"
            "  --heartbeat FILE     软狗心跳文件（应用写同一路径）\n"
            "  --heartbeat-timeout SEC  心跳过期秒数（默认 %d）\n"
            "  --heartbeat-startup SEC  启动宽限秒数（默认 %d）\n"
            "  --status FILE        状态文件（默认不写）\n"
            "  --pidfile FILE       单实例锁 + PID（默认 %s）\n"
            "  --daemon             转入后台运行\n",
            self, DEFAULT_MAX_RESTARTS, DEFAULT_WINDOW_SEC, DEFAULT_GRACE_SEC,
            DEFAULT_RESTART_DELAY_MS, DEFAULT_SAFE_CMD, DEFAULT_WATCHDOG_DEV,
            DEFAULT_LOG_MAX, DEFAULT_HB_TIMEOUT_SEC, DEFAULT_HB_STARTUP_SEC,
            DEFAULT_PIDFILE);
}

int main(int argc, char ** argv)
{
    const char * app = NULL;
    const char * safe_cmd = DEFAULT_SAFE_CMD;
    const char * wd_dev = DEFAULT_WATCHDOG_DEV;
    const char * log_path = NULL;
    const char * status_path = NULL;
    const char * pidfile = DEFAULT_PIDFILE;
    const char * app_log = NULL;
    int max_restarts = DEFAULT_MAX_RESTARTS;
    int window = DEFAULT_WINDOW_SEC;
    int grace = DEFAULT_GRACE_SEC;
    int restart_delay_ms = DEFAULT_RESTART_DELAY_MS;
    int want_daemon = 0;
    int app_arg_start = -1;      /* '--' 之后第一个应用参数的下标 */

    /* ---- 参数解析 ----
     * ★ 先完整解析再组装 app_argv：旧实现遇到 '--' 就 break，若 '--' 出现在
     *   --app 之前，app 仍是 NULL，随后的 app_argv[0] = app 会写入 NULL，
     *   execvp 直接崩在 supervisor 里（表现为「守护进程自己起不来」）。 */
    for(int i = 1; i < argc; i++) {
        const char * a = argv[i];
        if(strcmp(a, "--") == 0) { app_arg_start = i + 1; break; }
        if(strcmp(a, "--app") == 0 && i + 1 < argc) { app = argv[++i]; continue; }
        if(strcmp(a, "--max-restarts") == 0 && i + 1 < argc) { max_restarts = atoi(argv[++i]); continue; }
        if(strcmp(a, "--window") == 0 && i + 1 < argc) { window = atoi(argv[++i]); continue; }
        if(strcmp(a, "--grace") == 0 && i + 1 < argc) { grace = atoi(argv[++i]); continue; }
        if(strcmp(a, "--restart-delay") == 0 && i + 1 < argc) { restart_delay_ms = atoi(argv[++i]); continue; }
        if(strcmp(a, "--safe-cmd") == 0 && i + 1 < argc) { safe_cmd = argv[++i]; continue; }
        if(strcmp(a, "--watchdog") == 0 && i + 1 < argc) { wd_dev = argv[++i]; continue; }
        if(strcmp(a, "--app-log") == 0 && i + 1 < argc) { app_log = argv[++i]; continue; }
        if(strcmp(a, "--heartbeat") == 0 && i + 1 < argc) { g_hb_path = argv[++i]; continue; }
        if(strcmp(a, "--heartbeat-timeout") == 0 && i + 1 < argc) { g_hb_timeout_s = atoi(argv[++i]); continue; }
        if(strcmp(a, "--heartbeat-startup") == 0 && i + 1 < argc) { g_hb_startup_s = atoi(argv[++i]); continue; }
        if(strcmp(a, "--log") == 0 && i + 1 < argc) { log_path = argv[++i]; continue; }
        if(strcmp(a, "--log-max") == 0 && i + 1 < argc) { g_log_max = atol(argv[++i]); continue; }
        if(strcmp(a, "--status") == 0 && i + 1 < argc) { status_path = argv[++i]; continue; }
        if(strcmp(a, "--pidfile") == 0 && i + 1 < argc) { pidfile = argv[++i]; continue; }
        if(strcmp(a, "--daemon") == 0) { want_daemon = 1; continue; }
        if(strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        fprintf(stderr, "[ERROR] 未知参数: %s\n", a);
        usage(argv[0]);
        return 1;
    }

    if(app == NULL || *app == '\0') {
        fprintf(stderr, "[ERROR] 缺少 --app <应用路径>\n");
        usage(argv[0]);
        return 1;
    }
    if(access(app, X_OK) != 0) {
        fprintf(stderr, "[ERROR] 应用不存在或不可执行: %s (%s)\n", app, strerror(errno));
        return 1;
    }
    if(max_restarts < 0) max_restarts = 0;
    if(window <= 0)      window = DEFAULT_WINDOW_SEC;
    if(grace <= 0)       grace = DEFAULT_GRACE_SEC;
    if(restart_delay_ms < 0) restart_delay_ms = 0;
    if(g_hb_timeout_s <= 0) g_hb_timeout_s = DEFAULT_HB_TIMEOUT_SEC;
    if(g_hb_startup_s < 0)  g_hb_startup_s = 0;

    /* ---- 后台化（必须在打开日志/写 PID 之前，PID 才是最终进程） ---- */
    if(want_daemon) {
        if(log_path == NULL) {
            fprintf(stderr, "[WARN] --daemon 模式建议同时给 --log，否则日志会被 /dev/null 吞掉\n");
        }
        if(daemonize() != 0) {
            fprintf(stderr, "[ERROR] 转后台失败: %s\n", strerror(errno));
            return 1;
        }
    }

    /* ---- 单实例（重复启动不抢应用与看门狗） ---- */
    g_lock_fd = single_instance(pidfile);
    if(g_lock_fd == -2) {
        fprintf(stderr, "[ERROR] 已有 supervisor 在运行（锁文件 %s）\n", pidfile);
        return 3;
    }
    if(g_lock_fd < 0) {
        fprintf(stderr, "[WARN] 无法创建锁文件 %s: %s（继续运行，但无单实例保护）\n",
                pidfile, strerror(errno));
    }

    /* ---- 日志：先打开再装信号，保证「启动」这条一定落盘 ---- */
    if(log_path != NULL) {
        g_log_path = log_path;
        g_log = fopen(log_path, "a");
        if(g_log == NULL) fprintf(stderr, "[WARN] 无法打开日志 %s，回退 stderr\n", log_path);
    }
    g_status_path = status_path;

    /* ---- 信号：SIGTERM/SIGINT 停机并转发给应用；SIGPIPE 忽略 ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_line("supervisor 启动：app=%s pid=%d max-restarts=%d/%ds grace=%ds daemon=%d",
             app, (int)getpid(), max_restarts, window, grace, want_daemon);
    if(g_hb_path != NULL) {
        log_line("软狗：心跳=%s 超时=%ds 启动宽限=%ds",
                 g_hb_path, g_hb_timeout_s, g_hb_startup_s);
    }
    status_write("running", "-");

    /* 组装子进程 argv：'--' 位置替换为应用路径。 */
    char ** app_argv = NULL;
    if(app_arg_start >= 0) {
        app_argv = &argv[app_arg_start - 1];
        app_argv[0] = (char *)app;
    }
    else {
        static char * single[2];
        single[0] = (char *)app;
        single[1] = NULL;
        app_argv = single;
    }

    int rc = 0;
    while(!g_stop) {
        pid_t pid = fork();
        if(pid < 0) {
            log_line("fork 失败: %s", strerror(errno));
            rc = 1;
            break;
        }
        if(pid == 0) {
            /* 子进程：应用日志独立落盘（--daemon 下 supervisor 的 stdio 是 /dev/null，
             * 不重定向就等于把应用日志丢了）。O_APPEND 保证多次重启接续写。 */
            if(app_log != NULL && *app_log != '\0') {
                int fd = open(app_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if(fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    dup2(fd, STDERR_FILENO);
                    if(fd > STDERR_FILENO) close(fd);
                }
            }
            /* 子进程：告知应用自己处在守护之下，便于日志区分「手工启动」与「被守护」。 */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", g_nrestart);
            setenv("SAFE_SUPERVISOR", "1", 1);
            setenv("SAFE_RESTART_COUNT", buf, 1);
            execvp(app, app_argv);
            fprintf(stderr, "[supervisor] exec %s 失败: %s\n", app, strerror(errno));
            _exit(127);
        }

        g_child = pid;
        log_line("启动应用 pid=%d（第 %d 次）", (int)pid, g_nrestart);
        status_write("running", "running");

        int st = 0;
        int waited_ms = 0;
        pid_t r = 0;

        /* 软狗状态（每个子进程一份）：
         *   child_started_ms —— 启动宽限起点
         *   last_check_ms    —— 上次检查时间（1s 一次，别每 100ms stat）
         *   hb_hit/hb_killed —— 已 TERM / 已 KILL，防止每个轮询都重复发信号 */
        long long child_started_ms = monotonic_ms();
        long long last_check_ms = 0;
        long long hb_hit_ms = 0;
        int hb_hit = 0;
        int hb_killed = 0;

        for(;;) {
            r = waitpid(pid, &st, WNOHANG);
            if(r == pid) break;
            if(r < 0 && errno != EINTR) {
                log_line("waitpid 失败: %s", strerror(errno));
                rc = 1;
                break;
            }
            /* 停机中：宽限 grace 秒走优雅退出，超时强杀（防止应用忽略 SIGTERM 把 supervisor 挂住） */
            if(g_stop && waited_ms >= grace * 1000) {
                log_line("优雅退出宽限 %ds 超时 → SIGKILL pid=%d", grace, (int)pid);
                kill(pid, SIGKILL);
            }

            /* 软狗：应用没有真狗时，用「心跳文件是否还在更新」判断主循环是否卡死。 */
            long long nowm = monotonic_ms();
            if(g_hb_path != NULL && !hb_hit && nowm - last_check_ms >= 1000) {
                last_check_ms = nowm;
                long long age = 0;
                int hs = hb_check(child_started_ms, &age);
                if(hs == 1) {
                    log_line("心跳超时：%lldms 未更新（阈值 %ds）→ 判定应用卡死，SIGTERM pid=%d",
                             age, g_hb_timeout_s, (int)pid);
                    status_write("running", "heartbeat-timeout");
                    kill(pid, SIGTERM);
                    hb_hit = 1;
                    hb_hit_ms = nowm;
                }
                else if(hs == -1) {
                    log_line("心跳文件尚未出现（启动宽限 %ds 内，继续等待）", g_hb_startup_s);
                }
            }
            if(hb_hit && !hb_killed && nowm - hb_hit_ms >= (long long)grace * 1000LL) {
                log_line("心跳超时后优雅退出宽限 %ds 超时 → SIGKILL pid=%d", grace, (int)pid);
                kill(pid, SIGKILL);
                hb_killed = 1;
            }

            sleep_ms(POLL_MS);
            waited_ms += POLL_MS;
        }
        g_child = -1;
        if(r != pid) break;   /* waitpid 出错，退出循环 */

        /* ---- 退出原因分类 ----
         * ★ hb_hit 必须优先判断：应用被软狗 SIGTERM 后可能「优雅退出 code=0」，
         *   若按正常退出处理就不会重启 —— 卡死的应用被拉起来一次就再也不管了。 */
        char exit_desc[64];
        if(hb_hit) {
            snprintf(exit_desc, sizeof(exit_desc), "heartbeat-timeout");
        }

        if(WIFEXITED(st)) {
            int code = WEXITSTATUS(st);
            if(hb_hit) {
                log_line("应用在心跳超时后退出(code=%d)，按异常处理并重启", code);
            }
            else {
                snprintf(exit_desc, sizeof(exit_desc), "exit:%d", code);
                if(code == 0) {
                    log_line("应用正常退出(code=0)，不重启");
                    status_write("stopped", exit_desc);
                    rc = 0;
                    break;
                }
                log_line("应用异常退出 code=%d", code);
            }
        }
        else if(WIFSIGNALED(st)) {
            int sig = WTERMSIG(st);
            if(!hb_hit) {
                snprintf(exit_desc, sizeof(exit_desc), "signal:%s(%d)", signal_name(sig), sig);
            }
            if((sig == SIGTERM || sig == SIGINT) && !hb_hit) {
                log_line("应用被 %s 停止（视为正常停止），不重启", signal_name(sig));
                status_write("stopped", exit_desc);
                rc = 0;
                break;
            }
            log_line("应用被 %s(%d) 杀死（%s）", signal_name(sig), sig,
                     hb_hit ? "心跳超时后终止" : "异常");
        }
        else {
            if(!hb_hit) snprintf(exit_desc, sizeof(exit_desc), "unknown");
            log_line("应用以未知状态退出 st=%d", st);
        }

        if(g_stop) {
            log_line("停机中，不再重启");
            status_write("stopped", exit_desc);
            rc = 0;
            break;
        }

        time_t now = time(NULL);
        record_restart(now);
        int inwin = count_in_window(now, window);
        log_line("重启统计：窗口 %ds 内 %d 次（上限 %d）", window, inwin, max_restarts);
        if(inwin > max_restarts) {
            enter_safe_state(safe_cmd, wd_dev, "短时间内反复崩溃，判定设备异常");
            rc = 2;
            break;
        }

        /* 指数退避：第 n 次连续崩溃等 delay * 2^(n-1)，上限 8 倍。
         * 用窗口内次数而非总次数，避免「运行很久后偶发一次崩溃也等 8 倍」。 */
        int factor = 1;
        for(int i = 1; i < inwin && factor < BACKOFF_MAX_FACTOR; i++) factor *= 2;
        if(factor > BACKOFF_MAX_FACTOR) factor = BACKOFF_MAX_FACTOR;
        int delay = restart_delay_ms * factor;
        if(delay > 0) {
            log_line("本次重启间隔 %dms（退避 x%d）", delay, factor);
            sleep_ms(delay);
        }
    }

    log_line("supervisor 退出 rc=%d", rc);
    status_write(rc == 2 ? "safe" : "stopped", rc == 2 ? "safe-state" : "supervisor-exit");
    if(g_log != NULL) fclose(g_log);
    if(g_lock_fd >= 0) close(g_lock_fd);
    return rc;
}
