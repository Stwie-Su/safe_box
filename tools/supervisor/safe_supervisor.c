/**
 * @file safe_supervisor.c
 * 应用守护进程（方案 B：独立 supervisor 进程，不链接任何应用代码）。
 *
 * 【职责】
 *   1) fork + exec 应用，waitpid 监控退出状态；
 *   2) 区分「正常退出」与「异常崩溃」——正常退出 / 被 SIGTERM、SIGINT 停止 → 不重启；
 *      非 0 退出码，或被 SIGSEGV、SIGABRT 等杀死 → 重启；
 *   3) 重启限速：滑动时间窗内重启次数超过阈值 → 判定设备异常 → 进入安全态（不再重启）；
 *   4) 安全态（fail-safe）：执行安全动作钩子——默认 /etc/safe/safe_state.sh，
 *      真锁接入后指向「拉低 GPIO、保持锁闭」的命令；钩子不存在则只记录，不静默吞掉；
 *   5) 优雅停机：supervisor 收到 SIGTERM/SIGINT → 转发 SIGTERM 给应用
 *      （让应用走自己的优雅退出链：停流 → 关 fd → join），宽限 grace 秒仍未退出则 SIGKILL。
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
 *   --max-restarts N     滑动窗口内允许的重启次数（默认 5）
 *   --window SEC         滑动窗口长度，秒（默认 60）
 *   --grace SEC          优雅停机宽限，秒（默认 5）
 *   --restart-delay MS   两次重启之间的间隔，毫秒（默认 500）
 *   --safe-cmd CMD       进入安全态时执行的命令（默认 /etc/safe/safe_state.sh）
 *   --log FILE           日志落盘（默认 stderr）
 *
 * 退出码：0=应用正常退出（未重启）；2=进入安全态；1=参数或启动错误。
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MAX_RESTARTS      5
#define DEFAULT_WINDOW_SEC        60
#define DEFAULT_GRACE_SEC         5
#define DEFAULT_RESTART_DELAY_MS  500
#define DEFAULT_SAFE_CMD          "/etc/safe/safe_state.sh"
#define MAX_TRACKED               64
#define POLL_MS                   100

static volatile sig_atomic_t g_stop = 0;
static volatile pid_t        g_child = -1;
static FILE *                g_log = NULL;

static time_t g_restarts[MAX_TRACKED];
static int    g_nrestart = 0;

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

/* ------------------------------------------------------------------
 * 函数：enter_safe_state
 * 功能：进入安全态——执行安全动作钩子并停止重启。
 * 说明：钩子不存在/不可执行时只记录，绝不静默吞掉（安全设备的硬要求：失效必须导向安全侧且有痕）。
 * ------------------------------------------------------------------ */
static void enter_safe_state(const char * safe_cmd, const char * why)
{
    log_line("SAFE-STATE: %s（停止重启，设备保持安全态）", why);
    if(safe_cmd == NULL || *safe_cmd == '\0') return;
    if(access(safe_cmd, X_OK) == 0) {
        log_line("执行安全动作: %s", safe_cmd);
        int rc = system(safe_cmd);
        log_line("安全动作返回 rc=%d", rc);
    }
    else {
        log_line("安全动作不可用（不存在或不可执行）: %s —— 仅记录，不重启", safe_cmd);
    }
}

static void usage(const char * self)
{
    fprintf(stderr,
            "用法: %s --app <path> [选项] [-- 应用参数...]\n"
            "  --max-restarts N     滑动窗口内允许的重启次数（默认 %d）\n"
            "  --window SEC         滑动窗口长度（默认 %d）\n"
            "  --grace SEC          优雅停机宽限（默认 %d）\n"
            "  --restart-delay MS   重启间隔（默认 %d）\n"
            "  --safe-cmd CMD       安全态动作（默认 %s）\n"
            "  --log FILE           日志落盘（默认 stderr）\n",
            self, DEFAULT_MAX_RESTARTS, DEFAULT_WINDOW_SEC, DEFAULT_GRACE_SEC,
            DEFAULT_RESTART_DELAY_MS, DEFAULT_SAFE_CMD);
}

int main(int argc, char ** argv)
{
    const char * app = NULL;
    const char * safe_cmd = DEFAULT_SAFE_CMD;
    const char * log_path = NULL;
    int max_restarts = DEFAULT_MAX_RESTARTS;
    int window = DEFAULT_WINDOW_SEC;
    int grace = DEFAULT_GRACE_SEC;
    int restart_delay_ms = DEFAULT_RESTART_DELAY_MS;
    char ** app_argv = NULL;

    /* ---- 参数解析（应用参数用 -- 分隔） ---- */
    for(int i = 1; i < argc; i++) {
        const char * a = argv[i];
        if(strcmp(a, "--") == 0) {
            app_argv = &argv[i];          /* 以 '--' 位置起，稍后把 [0] 换成 app 路径 */
            app_argv[0] = (char *)app;
            break;
        }
        if(strcmp(a, "--app") == 0 && i + 1 < argc) { app = argv[++i]; continue; }
        if(strcmp(a, "--max-restarts") == 0 && i + 1 < argc) { max_restarts = atoi(argv[++i]); continue; }
        if(strcmp(a, "--window") == 0 && i + 1 < argc) { window = atoi(argv[++i]); continue; }
        if(strcmp(a, "--grace") == 0 && i + 1 < argc) { grace = atoi(argv[++i]); continue; }
        if(strcmp(a, "--restart-delay") == 0 && i + 1 < argc) { restart_delay_ms = atoi(argv[++i]); continue; }
        if(strcmp(a, "--safe-cmd") == 0 && i + 1 < argc) { safe_cmd = argv[++i]; continue; }
        if(strcmp(a, "--log") == 0 && i + 1 < argc) { log_path = argv[++i]; continue; }
        if(strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
    }

    if(app == NULL) {
        fprintf(stderr, "[ERROR] 缺少 --app <应用路径>\n");
        usage(argv[0]);
        return 1;
    }
    if(app_argv == NULL) {
        static char * single[2];
        single[0] = (char *)app;
        single[1] = NULL;
        app_argv = single;
    }
    else {
        app_argv[0] = (char *)app;
    }

    if(log_path != NULL) {
        g_log = fopen(log_path, "a");
        if(g_log == NULL) fprintf(stderr, "[WARN] 无法打开日志 %s，回退 stderr\n", log_path);
    }

    /* ---- 信号：SIGTERM/SIGINT 停机并转发给应用；SIGPIPE 忽略 ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_line("supervisor 启动：app=%s max-restarts=%d/%ds grace=%ds", app, max_restarts, window, grace);

    int rc = 0;
    while(!g_stop) {
        pid_t pid = fork();
        if(pid < 0) {
            log_line("fork 失败: %s", strerror(errno));
            rc = 1;
            break;
        }
        if(pid == 0) {
            execvp(app, app_argv);
            fprintf(stderr, "[supervisor] exec %s 失败: %s\n", app, strerror(errno));
            _exit(127);
        }

        g_child = pid;
        log_line("启动应用 pid=%d", (int)pid);

        int st = 0;
        int waited_ms = 0;
        pid_t r = 0;
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
            usleep(POLL_MS * 1000);
            waited_ms += POLL_MS;
        }
        g_child = -1;
        if(r != pid) break;   /* waitpid 出错，退出循环 */

        /* ---- 退出原因分类 ---- */
        if(WIFEXITED(st)) {
            int code = WEXITSTATUS(st);
            if(code == 0) {
                log_line("应用正常退出(code=0)，不重启");
                rc = 0;
                break;
            }
            log_line("应用异常退出 code=%d", code);
        }
        else if(WIFSIGNALED(st)) {
            int sig = WTERMSIG(st);
            if(sig == SIGTERM || sig == SIGINT) {
                log_line("应用被 SIG%s 停止（视为正常停止），不重启", (sig == SIGTERM) ? "TERM" : "INT");
                rc = 0;
                break;
            }
            log_line("应用被信号 %d 杀死（异常）", sig);
        }

        if(g_stop) {
            log_line("停机中，不再重启");
            rc = 0;
            break;
        }

        time_t now = time(NULL);
        record_restart(now);
        int inwin = count_in_window(now, window);
        log_line("重启统计：窗口 %ds 内 %d 次（上限 %d）", window, inwin, max_restarts);
        if(inwin > max_restarts) {
            enter_safe_state(safe_cmd, "短时间内反复崩溃，判定设备异常");
            rc = 2;
            break;
        }
        if(restart_delay_ms > 0) usleep((useconds_t)restart_delay_ms * 1000);
    }

    log_line("supervisor 退出 rc=%d", rc);
    if(g_log != NULL) fclose(g_log);
    return rc;
}
