/**
 * @file face_thread.c
 * face 线程实现（Sprint3 步骤 3b，规约 §3.4 / §3.3 / §5.19）。
 *
 * 职责：单线程 poll() 收敛两路节拍不同的 IO。
 *   - V4L2 video fd：可读 -> 取最新帧 -> YUYV->RGB565（转换在 camera 后端内完成）
 *     -> 拷贝进双帧缓冲发布（新帧覆盖旧帧，满则丢最旧，绝不阻塞采集）；
 *   - FM225 UART fd（3c 已接入）：可读 -> 读走字节 -> backend_fm225 的
 *     fm225_backend_feed() 喂协议状态机 -> 帧回调 emit -> event_bus_post 交主线程。
 *
 * 线程交接：
 *   - 预览帧：写双帧缓冲，主线程 page_face 经 face_thread_get_preview() 取最新；
 *   - 识别结果：经 event_bus_post(EV_FACE_EVENT) 交主线程（3c 接入；本步无识别源）；
 *   - 全程不碰 LVGL（§3.1）。
 *
 * 退出顺序（§3.2 / b5）：STREAMOFF 停流 -> close(fd) -> pthread_join。
 *   一把 io 锁把「采集临界区」与「request_stop 静默」串起来：request_stop 先置退出标志，
 *   再 STREAMOFF 唤醒 poll，然后取 io 锁再放开，保证返回时已无在途采集读到 mmap 缓冲，
 *   之后主线程关 fd（hal_camera_deinit）才安全，最后 join 回收线程，避免 DQBUF 卡住。
 *
 * 后端驱动去向（b3 取简）：hal_face_tick 仍留在主线程（app_tick_fast）。
 *   理由：后端 tick（fake 计时 / enroll 超时）读写 face_service 内部状态，且与主线程的
 *   inject/enroll/delete 控制路径共享；移入本线程需给 face_service 全面加锁却无功能收益。
 *   本步只把「相机采集 + 格式转换」这一真正的重活从主线程搬到本线程——b3 验收要求
 *   「主线程不再做帧转换」，已由 page_face 改读本线程缓冲满足。3c 实现 FM225 协议时，
 *   串口字节由本线程 poll 唤醒后经后端 feed 接口驱动状态机，识别结果走 event_bus_post。
 *
 * 线程同步：退出标志用 volatile 轮询（跨线程仅此一个标量，其余共享状态一律走互斥量），
 *   采集临界区与关 fd 的次序由 io 锁保证；双帧缓冲由 fb 锁保证。
 */

#include "platform/thread_util.h"      /* ★ 必须第一个 include：它要在 <pthread.h> 前定义 _GNU_SOURCE */

#include "hal/face/face_thread.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hal/hal_camera.h"
#include "hal/hal_face.h"

#define FACE_POLL_TIMEOUT_MS  20        /* 无 IO 时的兜底节拍，保证能及时感知退出 */

#define FB_PIXELS  (FACE_THREAD_PREVIEW_W * FACE_THREAD_PREVIEW_H)
#define FB_BYTES   ((size_t)FB_PIXELS * 2u)   /* RGB565 */

static pthread_t   s_thread;
static bool        s_created;                  /* 线程已创建且未 join */
static volatile bool s_quit;                   /* 退出标志（主线程置，线程读） */
static volatile bool s_running;                /* 线程主体是否在跑 */

/* FM225 UART fd：-1 = 未接入（poll 忽略负 fd）。仅 start 前可设置。 */
static int         s_uart_fd = -1;

/* ==================================================================
 * 【双帧缓冲（本模块的核心数据结构）】—— 「单生产者 + 单消费者」经典解法
 *
 *    s_fb[0]        s_fb[1]          两块各 320×240×2 = 150KB
 *   ┌────────┐    ┌────────┐
 *   │  帧 N  │    │ (空闲) │        s_fb_pub = 0 表示「0 号是当前可读的」
 *   └────────┘    └────────┘
 *       ▲              ▲
 *       │读（fb 锁）    │写（无锁）
 *   主线程（消费者）   face 线程（生产者）
 *
 * 规则三条：
 *   1) 生产者永远写 `s_fb_pub ^ 1`（另一块），绝不碰消费者正在读的那块
 *      → 拷贝/转换期间不需要持锁，锁只在「翻 s_fb_pub 指针」那一瞬间持有；
 *   2) 消费者（get_preview）在 fb 锁下 memcpy 走 s_fb[s_fb_pub]，然后置
 *      s_fb_consumed = s_fb_seq，表示「这块我读过了」；
 *   3) 若消费者还没来读，生产者又产出新帧 → 直接覆盖再下一块 = **丢最旧帧**。
 *      这是刻意设计：预览宁可丢帧，也绝不让采集阻塞（阻塞会导致延迟累积）。
 *
 * 对比「环形队列 + 条件变量」：这里只有 2 块、且允许丢帧，
 * 故用「双缓冲 + 序号」比用队列 + 信号量更轻，也不会有唤醒抖动。
 * ================================================================== */

/* 双帧缓冲：主线程在 fb 锁下读 s_fb[s_fb_pub]；本线程把新帧写进另一块（锁外），
 * 写完再上锁发布（s_fb_pub 翻到新块）。生产者永不写消费者正在读的那块，故无需在
 * 转换/拷贝期间持锁；未被消费的旧帧在下一帧发布时被覆盖——即「满则丢最旧」。 */
static uint16_t    s_fb[2][FB_PIXELS];
static int         s_fb_pub;                   /* 已发布（可读）缓冲区下标 */
static uint32_t    s_fb_seq;                   /* 发布序号 */
static uint32_t    s_fb_consumed;              /* 主线程已消费到的序号 */
static hal_camera_frame_info_t s_fb_info;
static pthread_mutex_t s_fb_lock = PTHREAD_MUTEX_INITIALIZER;

/* io 锁：串起「在途采集临界区」与「request_stop 静默」，保证关 fd 前无采集在读 mmap。 */
static pthread_mutex_t s_io_lock = PTHREAD_MUTEX_INITIALIZER;

/* 「分辨率不符」告警去重：记住上次已告警的协商分辨率（初值 0 保证首次不符必打印），
 * 仅当本次协商值与上次不同才再打印一次，避免每帧刷屏。仅本线程（采集临界区内）读写。 */
static unsigned s_warn_w = 0;
static unsigned s_warn_h = 0;

/* ---------------- 采集 ---------------- */

/* ------------------------------------------------------------------
 * 函数：face_capture_once（文件内私有）★生产者侧核心
 * 功能：取一帧、拷进双帧缓冲的空闲块、发布。一次调用 = 一帧。
 * 调用者：face_thread_main()（poll 检测到相机 fd 可读时）。
 *
 * 【数据流】
 *   hal_camera_frame()          ← 后端内部已完成 DQBUF + YUYV/MJPG→RGB565
 *        │ src 指向后端内部缓冲（**非拷贝**，靠 release 归还）
 *        ├─ 分辨率 == 320×240？
 *        │    ├─ 是 → memcpy 到 s_fb[s_fb_pub ^ 1] → release() → 持 fb 锁翻 s_fb_pub、++s_fb_seq
 *        │    └─ 否 → release() 后**丢弃**（不拷贝，防止按定长 FB_BYTES 读穿 src 末端）
 *        ▼
 *   发布完成，主线程下一次 get_preview 就能取到
 *
 * 【并发要点】整段包在 io 锁内：request_stop() 会「抢一次 io 锁再放开」，
 *   那次加锁会一直等到本函数退出临界区，从而保证「关 fd 时没有人在读 mmap」。
 * 【纪律】临界区内不做重计算、不调用可能阻塞的外部函数（printf 就属于此类，
 *   它可能因 stdio 锁或缓冲区 flush 而阻塞）——本函数把告警 printf 留到解锁之后，
 *   正是这条纪律的体现。
 * ------------------------------------------------------------------ */
static void face_capture_once(void)
{
    bool     warn   = false;            /* 本次是否需要打印「分辨率不符」告警 */
    unsigned warn_w = 0;                /* 待打印的协商分辨率（锁内记下，解锁后用） */
    unsigned warn_h = 0;

    pthread_mutex_lock(&s_io_lock);
    if (!s_quit) {
        const uint8_t * src = NULL;
        hal_camera_frame_info_t info;
        memset(&info, 0, sizeof(info));
        safe_err_t e = hal_camera_frame(&src, &info);
        if (e == SAFE_OK && src != NULL) {
            /* src 的实际有效长度 = info.width*info.height*2，而 FB_BYTES 是编译期定长
             * （预览缓冲）。仅当驱动协商出的分辨率与预览缓冲一致时才拷贝，否则按定长
             * FB_BYTES 读取会越过 src 末端（读穿 mmap），故必须前置校验。 */
            if (info.width == FACE_THREAD_PREVIEW_W &&
                info.height == FACE_THREAD_PREVIEW_H) {
                int slot = s_fb_pub ^ 1;
                memcpy(s_fb[slot], src, FB_BYTES);   /* 写非发布块，不打断消费者 */
                hal_camera_release_frame();

                pthread_mutex_lock(&s_fb_lock);
                s_fb_pub  = slot;
                s_fb_info = info;
                s_fb_seq++;
                pthread_mutex_unlock(&s_fb_lock);
            } else {
                /* 不符：先按 §5.13 接口约定释放帧（不释放后端会停更），丢弃该帧（不拷贝，
                 * 杜绝越界读）。去重状态 s_warn_w/s_warn_h 只在临界区内更新（仅本线程写，
                 * 无竞态），但告警 printf 本身留到解锁之后执行——§3.1 明文要求「临界区内
                 * 不做重计算、不持锁调用外部函数」：printf 可能因 stdio 锁/缓冲 flush 阻塞，
                 * 持 s_io_lock 阻塞会推迟 request_stop() 的静默收尾，属已知反模式。 */
                hal_camera_release_frame();
                if (info.width != s_warn_w || info.height != s_warn_h) {
                    s_warn_w = info.width;
                    s_warn_h = info.height;
                    warn     = true;
                    warn_w   = info.width;
                    warn_h   = info.height;
                }
            }
        }
    }
    pthread_mutex_unlock(&s_io_lock);

    /* 解锁后再打印：不持 io 锁调用可能阻塞的 stdio（§3.1）。 */
    if (warn) {
        printf("[FACE-THREAD] 协商分辨率 %ux%u 与预览缓冲 %dx%d 不符，丢弃该帧\n",
               warn_w, warn_h,
               (unsigned)FACE_THREAD_PREVIEW_W, (unsigned)FACE_THREAD_PREVIEW_H);
    }
}

/* ---------------- FM225 UART（步骤 3c 接入） ---------------- */

/* 后端提供的字节流入口（backend_fm225.c）：喂协议状态机后由后端帧回调
 * emit 识别结果（event_bus_post 交主线程）。fd 只借用，归后端所有。 */
void fm225_backend_feed(const uint8_t * buf, size_t n);

/* ------------------------------------------------------------------
 * 函数：face_uart_drain（文件内私有）
 * 功能：把串口里当前可读的字节**一次读干净**，喂给 FM225 协议状态机。
 * 调用者：face_thread_main()（poll 检测到串口 fd 可读时）。
 * 数据流：read(s_uart_fd) → 256 字节栈缓冲 → fm225_backend_feed() → 后端状态机
 *         → 识别结果经 event_bus_post 交回主线程。**本函数不解析协议**。
 * 为什么用循环读到 n<=0：一次 poll 可读可能对应多帧串口数据；循环读空才能
 *   避免「数据来了却只处理一半、剩下的要等下一次事件」造成的延迟。
 *   n<=0 涵盖 EAGAIN（非阻塞读完）、EINTR（被信号打断）、EOF 三种情况，统一退出。
 * ------------------------------------------------------------------ */
static void face_uart_drain(void)
{
    if (s_uart_fd < 0) return;
    uint8_t buf[256];
    for (;;) {
        ssize_t n = read(s_uart_fd, buf, sizeof(buf));
        if (n <= 0) break;      /* EAGAIN（非阻塞 fd）/ EINTR / EOF */
        fm225_backend_feed((const uint8_t *)buf, (size_t)n);
    }
}

/* ---------------- 线程主体 ---------------- */

/* ------------------------------------------------------------------
 * 函数：face_thread_main ★线程主体（pthread 入口）
 * 功能：一个循环，用 poll() 同时等「相机来帧」和「串口来字节」，来谁处理谁。
 * 调用者：pthread_create（由 face_thread_start() 拉起）。
 *
 * 【循环结构】
 *     ① 检查 s_quit → 退出
 *     ② 组装 pollfd 数组：[0]=相机 fd（非前台时置 -1）/ [1]=串口 fd
 *     ③ poll(…, 20ms)   ← 超时是兜底节拍，保证退出标志最迟 20ms 内被感知
 *     ④ 再检查一次 s_quit → 退出（关键：poll 返回后立刻查，
 *        确保退出期间不再碰 fd——这是「关 fd 的前置条件」）
 *     ⑤ 哪个 fd 可读就调对应处理函数（face_capture_once / face_uart_drain）
 *
 * 【为什么用 poll 而不是两个线程各阻塞读】
 *   两路 IO 节拍完全不同（相机 20fps 连续流、串口事件驱动），
 *   一个线程 + IO 复用 就是标准解法：线程数不涨、无需在两路间加锁、退出可控。
 *   （本工程线程数封顶 4，见 face_thread.h 文件头。）
 *
 * 【Duty-cycle 处理】非前台时把相机 fd 设成 -1 移出 pollset：
 *   STREAMOFF 后该 fd 会持续 POLLERR 立即返回，不摘除就退化成 100% CPU 忙轮询。
 * ------------------------------------------------------------------ */
static void * face_thread_main(void * arg)
{
    safe_thread_setname("safe-face");
    (void)arg;
    s_running = true;

    for (;;) {
        if (s_quit) break;

        struct pollfd fds[2];
        /* Duty-cycle（FR-27）：前台暂停时把相机 fd 移出 pollset —— STREAMOFF 后
         * 该 fd 会持续 POLLERR 立即返回，不摘除会退化成忙轮询。 */
        fds[0].fd      = hal_face_foreground() ? hal_camera_fd() : -1;
        fds[0].events  = POLLIN;
        fds[0].revents = 0;
        fds[1].fd      = s_uart_fd;         /* FM225 未接时 -1 -> poll 忽略 */
        fds[1].events  = POLLIN;
        fds[1].revents = 0;

        int n = poll(fds, 2, FACE_POLL_TIMEOUT_MS);

        /* 退出优先：poll 返回后立刻检查，确保退出期间不再碰 fd（关 fd 的前置条件）。 */
        if (s_quit) break;

        if (n > 0) {
            if (fds[0].revents & (POLLIN | POLLPRI)) face_capture_once();
            if (fds[1].revents & (POLLIN | POLLPRI)) face_uart_drain();
        }
        /* POLLERR/POLLHUP（流停止）不特判：退出由 s_quit 驱动，poll 超时会再次返回。 */
    }

    s_running = false;
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

/* 函数头见 face_thread.h。此处只说实现要点：
 * 启动前把双帧缓冲与序号**全部清零**，是为了让消费端的 s_fb_seq != s_fb_consumed
 * 判据在线程重启后依然成立（否则可能误判「没有新帧」而永远不显示）。 */
safe_err_t face_thread_start(void)
{
    if (s_created) return SAFE_OK;

    s_quit        = false;
    s_fb_pub      = 0;
    s_fb_seq      = 0;
    s_fb_consumed = 0;
    memset(s_fb, 0, sizeof(s_fb));
    memset(&s_fb_info, 0, sizeof(s_fb_info));

    if (pthread_create(&s_thread, NULL, face_thread_main, NULL) != 0) {
        printf("[FACE-THREAD] pthread_create 失败\n");
        return SAFE_ERR_FAIL;
    }
    s_created = true;
    printf("[FACE-THREAD] 已启动（poll 双 fd，UART fd=%d）\n", s_uart_fd);
    return SAFE_OK;
}

/* 函数头见 face_thread.h。最后那两行「加锁立刻解锁」不是笔误：
 * 它会阻塞到当前持有 io 锁的采集临界区跑完，返回时即可保证「无在途采集」，
 * 主线程才能安全地 close(fd)。这是一种**用互斥量做 rendezvous（会合）**的写法。 */
void face_thread_request_stop(void)
{
    if (!s_created) return;
    s_quit = true;
    /* STREAMOFF：停流并唤醒可能阻塞在 poll 的 face 线程（POLLERR）。 */
    hal_camera_stop();
    /* 静默：等在途采集临界区退出，保证返回后无线程在读 mmap 缓冲。 */
    pthread_mutex_lock(&s_io_lock);
    pthread_mutex_unlock(&s_io_lock);
}

void face_thread_join(void)
{
    if (!s_created) return;
    pthread_join(s_thread, NULL);
    s_created = false;
}

bool face_thread_running(void)
{
    return s_running;
}

void face_thread_set_uart_fd(int fd)
{
    if (s_created) return;      /* 运行期不可改，避免与线程读竞争 */
    s_uart_fd = fd;
}

/* 函数头见 face_thread.h。实现要点：
 * 判据 `s_fb_seq != s_fb_consumed` 是「有比上次更新的帧吗」的全部依据——
 * 生产者每发布一帧 ++s_fb_seq，消费者取走后置 s_fb_consumed = s_fb_seq。
 * 注意消费者**不比较帧内容**，只比较序号，因此不存在「两帧看起来一样」的歧义。 */
bool face_thread_get_preview(uint16_t * dst, hal_camera_frame_info_t * info)
{
    if (dst == NULL) return false;
    bool ok = false;
    pthread_mutex_lock(&s_fb_lock);
    if (s_fb_seq != s_fb_consumed) {
        memcpy(dst, s_fb[s_fb_pub], FB_BYTES);
        if (info) *info = s_fb_info;
        s_fb_consumed = s_fb_seq;
        ok = true;
    }
    pthread_mutex_unlock(&s_fb_lock);
    return ok;
}



