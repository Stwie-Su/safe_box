/**
 * @file face_thread.c
 * face 线程实现（Sprint3 步骤 3b，规约 §3.4 / §3.3 / §5.19）。
 *
 * 职责：单线程 poll() 收敛两路节拍不同的 IO。
 *   - V4L2 video fd：可读 -> 取最新帧 -> YUYV->RGB565（转换在 camera 后端内完成）
 *     -> 拷贝进双帧缓冲发布（新帧覆盖旧帧，满则丢最旧，绝不阻塞采集）；
 *   - FM225 UART fd：可读 -> 读走字节喂协议状态机（3c 实现，本步仅留接入点，可为 -1）。
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
 * 后端驱动去向（b3 取简）：face_service_tick 仍留在主线程（app_tick_fast）。
 *   理由：后端 tick（fake 计时 / enroll 超时）读写 face_service 内部状态，且与主线程的
 *   inject/enroll/delete 控制路径共享；移入本线程需给 face_service 全面加锁却无功能收益。
 *   本步只把「相机采集 + 格式转换」这一真正的重活从主线程搬到本线程——b3 验收要求
 *   「主线程不再做帧转换」，已由 page_face 改读本线程缓冲满足。3c 实现 FM225 协议时，
 *   串口字节由本线程 poll 唤醒后经后端 feed 接口驱动状态机，识别结果走 event_bus_post。
 *
 * 线程同步：退出标志用 volatile 轮询（跨线程仅此一个标量，其余共享状态一律走互斥量），
 *   采集临界区与关 fd 的次序由 io 锁保证；双帧缓冲由 fb 锁保证。
 */

#include "hal/face/face_thread.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hal/hal_camera.h"

#define FACE_POLL_TIMEOUT_MS  20        /* 无 IO 时的兜底节拍，保证能及时感知退出 */

#define FB_PIXELS  (FACE_THREAD_PREVIEW_W * FACE_THREAD_PREVIEW_H)
#define FB_BYTES   ((size_t)FB_PIXELS * 2u)   /* RGB565 */

static pthread_t   s_thread;
static bool        s_created;                  /* 线程已创建且未 join */
static volatile bool s_quit;                   /* 退出标志（主线程置，线程读） */
static volatile bool s_running;                /* 线程主体是否在跑 */

/* FM225 UART fd：-1 = 未接入（poll 忽略负 fd）。仅 start 前可设置。 */
static int         s_uart_fd = -1;

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

/* 取一帧：camera 后端内部完成 DQBUF(只转最新) + YUYV->RGB565；
 * 拷贝到双帧缓冲的非发布块后发布。整段在 io 锁内，保证 request_stop 能等到它收尾。 */
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
                /* 不符：先按 §5.13 契约释放帧（不释放后端会停更），丢弃该帧（不拷贝，
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

/* ---------------- FM225 UART（3c 接入点） ---------------- */

/* 3c：把读到的字节喂 FM225 协议状态机，识别结果经 event_bus_post(EV_FACE_EVENT) 上报。
 * 本步仅把字节读走丢弃，避免串口缓冲涨满；未接入（fd<0）时不会走到这里。 */
static void face_uart_drain(void)
{
    if (s_uart_fd < 0) return;
    uint8_t buf[128];
    for (;;) {
        ssize_t n = read(s_uart_fd, buf, sizeof(buf));
        if (n <= 0) break;      /* EAGAIN / EINTR / EOF：状态机在 3c 处理 */
        /* TODO-FM225(3c)：fm225_proto_feed(ctx, buf, n) -> face_service_emit(...) */
    }
}

/* ---------------- 线程主体 ---------------- */

static void * face_thread_main(void * arg)
{
    (void)arg;
    s_running = true;

    for (;;) {
        if (s_quit) break;

        struct pollfd fds[2];
        fds[0].fd      = hal_camera_fd();   /* 无相机时 -1 -> poll 忽略 */
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



