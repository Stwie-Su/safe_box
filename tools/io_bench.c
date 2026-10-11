/**
 * @file io_bench.c
 * IO 多路复用微基准：select / poll / epoll(LT) / epoll(ET) 在不同 fd 规模下的单次等待开销。
 *
 * 目的：用数据回答「本项目要不要把 poll 换成 epoll」，而不是凭口号。
 *
 * 方法：
 *   - 用 pipe() 造 N 个可读端 fd 作为监听集合（pipe 是最轻量的可读事件源）；
 *   - 每次迭代：向 active 个 pipe 各写 1 字节 -> 等待 -> 把就绪的读回（保证下一轮干净）；
 *   - 只计时 wait 调用本身（多路复用的核心开销：系统调用 + 内核遍历/回调）。
 *
 * 前提（编译/运行）：
 *   - POSIX：pipe / select / poll / epoll / clock_gettime(CLOCK_MONOTONIC)
 *   - Linux（epoll 为 Linux 专有；select 受 FD_SETSIZE 限制，N 超过时自动跳过）
 *   - 编译：cc -O2 -o io_bench io_bench.c
 *   - 用法：./io_bench [iterations]   （默认 20000）
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* 场景：N 个 pipe，其中 active 个有数据 */
typedef struct {
    int n;
    int active;
    int * rd;      /* 读端（被监听） */
    int * wr;      /* 写端（注入数据） */
} scene_t;

static void scene_init(scene_t * s, int n, int active)
{
    s->n = n; s->active = active;
    s->rd = calloc(n, sizeof(int));
    s->wr = calloc(n, sizeof(int));
    for(int i = 0; i < n; i++) {
        int fds[2];
        if(pipe(fds) != 0) { perror("pipe"); exit(1); }
        s->rd[i] = fds[0];
        s->wr[i] = fds[1];
        /* ★ 读端必须是非阻塞：drain_one() 靠 read 返回 -1/EAGAIN 判断"读尽"，
         * 阻塞模式下读完最后一个字节会直接挂死（这也是 epoll ET 的标准要求）。 */
        if(fcntl(s->rd[i], F_SETFL, O_NONBLOCK) != 0) { perror("fcntl"); exit(1); }
    }
}

static void scene_free(scene_t * s)
{
    for(int i = 0; i < s->n; i++) { close(s->rd[i]); close(s->wr[i]); }
    free(s->rd); free(s->wr);
}

/* 注入 active 个字节（前 active 个 pipe 各 1 字节） */
static void scene_fire(scene_t * s)
{
    for(int i = 0; i < s->active; i++) {
        if(write(s->wr[i], "x", 1) != 1) { perror("write"); exit(1); }
    }
}

/* 把第 i 个 pipe 的数据读干净 */
static void drain_one(int fd)
{
    char buf[64];
    while(read(fd, buf, sizeof(buf)) > 0) { }
}

static double bench_select(scene_t * s, int iters)
{
    /* select 的两道硬门槛：
     *  ① 监听数量 > FD_SETSIZE；
     *  ② ★ fd 号本身 >= FD_SETSIZE —— fd_set 是**按 fd 号索引的位图**，不是按监听个数，
     *     FD_SET 一个 >= FD_SETSIZE 的 fd 会越界写，实测直接 segfault（不是理论风险）。
     *     这也是 select 在高 fd 号场景下的经典陷阱，故此处显式防护并标记 n/a。 */
    if(s->n > FD_SETSIZE) return -1.0;
    for(int i = 0; i < s->n; i++) if(s->rd[i] >= FD_SETSIZE) return -1.0;

    long long t0 = now_ns();
    for(int it = 0; it < iters; it++) {
        scene_fire(s);
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = 0;
        for(int i = 0; i < s->n; i++) {
            FD_SET(s->rd[i], &rfds);
            if(s->rd[i] > maxfd) maxfd = s->rd[i];
        }
        struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if(r < 0) { perror("select"); return -1.0; }
        for(int i = 0; i < s->n; i++) if(FD_ISSET(s->rd[i], &rfds)) drain_one(s->rd[i]);
    }
    return (double)(now_ns() - t0) / iters;
}

static double bench_poll(scene_t * s, int iters)
{
    struct pollfd * pfd = calloc(s->n, sizeof(struct pollfd));
    long long t0 = now_ns();
    for(int it = 0; it < iters; it++) {
        scene_fire(s);
        for(int i = 0; i < s->n; i++) { pfd[i].fd = s->rd[i]; pfd[i].events = POLLIN; pfd[i].revents = 0; }
        int r = poll(pfd, s->n, 1000);
        if(r < 0) { perror("poll"); free(pfd); return -1.0; }
        for(int i = 0; i < s->n; i++) if(pfd[i].revents & POLLIN) drain_one(pfd[i].fd);
    }
    free(pfd);
    return (double)(now_ns() - t0) / iters;
}

static double bench_epoll(scene_t * s, int iters, int edge)
{
    int ep = epoll_create1(0);
    if(ep < 0) { perror("epoll_create1"); return -1.0; }
    struct epoll_event * ev = calloc(s->n, sizeof(struct epoll_event));
    for(int i = 0; i < s->n; i++) {
        ev[i].events = EPOLLIN | (edge ? EPOLLET : 0);
        ev[i].data.fd = s->rd[i];
        if(epoll_ctl(ep, EPOLL_CTL_ADD, s->rd[i], &ev[i]) != 0) { perror("epoll_ctl"); return -1.0; }
    }
    struct epoll_event * out = calloc(s->n, sizeof(struct epoll_event));
    long long t0 = now_ns();
    for(int it = 0; it < iters; it++) {
        scene_fire(s);
        int r = epoll_wait(ep, out, s->n, 1000);
        if(r < 0) { perror("epoll_wait"); return -1.0; }
        for(int i = 0; i < r; i++) drain_one(out[i].data.fd);
    }
    free(out); free(ev); close(ep);
    return (double)(now_ns() - t0) / iters;
}

int main(int argc, char ** argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 20000;
    if(iters <= 0) iters = 20000;

    /* fd 规模梯度：2（本项目现状量级）-> 1024 */
    int ns[]      = {2, 4, 8, 16, 64, 256, 1024};
    int nn        = sizeof(ns) / sizeof(ns[0]);
    int actives[] = {1, 1, 1, 2, 4, 8, 16};

    printf("IO 多路复用微基准  iters=%d  计时=单次 wait 循环(ns，含注入与排空)\n", iters);
    printf("%6s %8s %12s %12s %12s %12s\n", "n_fd", "active", "select", "poll", "epoll_LT", "epoll_ET");
    for(int k = 0; k < nn; k++) {
        scene_t s;
        scene_init(&s, ns[k], actives[k] > ns[k] ? ns[k] : actives[k]);
        double r_sel = bench_select(&s, iters);
        double r_poll = bench_poll(&s, iters);
        double r_lt = bench_epoll(&s, iters, 0);
        double r_et = bench_epoll(&s, iters, 1);
        char sel_buf[32];
        if(r_sel < 0) snprintf(sel_buf, sizeof(sel_buf), "n/a");
        else          snprintf(sel_buf, sizeof(sel_buf), "%.0f", r_sel);
        printf("%6d %8d %12s %12.0f %12.0f %12.0f\n",
               ns[k], s.active, sel_buf, r_poll, r_lt, r_et);
        scene_free(&s);
    }
    return 0;
}

