/**
 * @file camera_v4l2.c
 * V4L2 摄像头后端（Sprint3 步骤 2，FR-16 视频预览）。
 *
 * 设计要点（对应规约 §4.2 性能预算 / §5.13 相机契约）：
 *  1. V4L2 mmap 零拷贝采集：驱动缓冲直接映射，DQBUF 后在用户态做
 *     YUYV→RGB565 查表转换（整型，无逐像素浮点），再 QBUF 还回驱动；
 *  2. 采集 = 显示 = 320×240（需求 v1.6 定案，1:1 直通无缩放开销）。
 *     start(w,h) 传期望值，驱动协商结果以 S_FMT 返回为准；
 *  3. 帧缓冲归后端所有（§5.13 契约）：frame() 返回内部转换缓冲，
 *     消费方必须调 release()，否则下一帧返回 SAFE_ERR_BUSY（后端停更）；
 *  4. 本步（步骤 2）转换发生在调用方线程（LVGL 主循环 20ms tick）；
 *     步骤 3 引入 face 线程后由 face 线程 poll 该 fd（§3.4）。
 *
 * 设备：自动探测 /dev/video0..15 中第一个支持采集 + 流式的节点（板子上 video0 常是 PxP 这类
 *       非采集节点，真摄像头落在 video1+）；可用环境变量 SAFE_CAMERA_DEV 显式覆盖。
 * 采集帧率：S_PARM 请求 30fps，驱动按能力收敛（实测 UVC 摄像头 YUYV@320x240 为 20fps）。
 */

#include "camera_backend.h"
#include "hal/hal_time.h"

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define CAM_BUF_COUNT   4
#define CAM_DEV_PATH_MAX 64

typedef struct {
    void  * start;
    size_t  length;
} v4l2_buf_t;

static int        s_fd = -1;
static bool       s_streaming;
static v4l2_buf_t s_bufs[CAM_BUF_COUNT];
static int        s_buf_count;

/* 协商后的采集格式 */
static uint16_t   s_cap_w, s_cap_h;
static uint32_t   s_seq;

/* YUYV → RGB565 输出缓冲（后端所有，frame() 交给消费方） */
static uint8_t  * s_out;
static size_t     s_out_size;
static bool       s_held;      /* 消费方未 release 时为 true */

/* Y/U/V 分量查表：init 时一次性用浮点算好，转换期纯整型加法 + 位运算 */
static int s_y_tab[256];       /* 1.164*(Y-16)          */
static int s_v_r_tab[256];     /* 1.596*(V-128)         */
static int s_u_g_tab[256];     /* -0.391*(U-128)        */
static int s_v_g_tab[256];     /* -0.813*(V-128)        */
static int s_u_b_tab[256];     /* 2.018*(U-128)         */
static bool s_tab_ready;

static uint16_t pack565(int r, int g, int b)
{
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    if (g < 0)   g = 0;
    if (g > 255) g = 255;
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void build_tables(void)
{
    if (s_tab_ready) return;
    for (int i = 0; i < 256; i++) {
        s_y_tab[i]   = (int)(1.164  * (i - 16));
        s_v_r_tab[i] = (int)(1.596  * (i - 128));
        s_u_g_tab[i] = (int)(-0.391 * (i - 128));
        s_v_g_tab[i] = (int)(-0.813 * (i - 128));
        s_u_b_tab[i] = (int)(2.018  * (i - 128));
    }
    s_tab_ready = true;
}

/* ioctl 带健壮重试：EINTR 重试，EINVAL/其它直接失败 */
static int xioctl(int fd, unsigned long req, void * arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* 是否为「可采集 + 支持流式」的摄像头。
 * 现代驱动在 capabilities 置 V4L2_CAP_DEVICE_CAPS 时必须看 device_caps（V4L2 约定），
 * 否则退回 capabilities。板子上的 PxP 是 M2M 图像处理节点，只有 VIDEO_OUTPUT，
 * 无 VIDEO_CAPTURE，会被正确排除。 */
static bool is_capture_dev(const struct v4l2_capability * cap)
{
    uint32_t caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? cap->device_caps : cap->capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

/* 选设备：SAFE_CAMERA_DEV 显式指定优先；否则按 /dev/video0..15 顺序探测，
 * 取**第一个真正支持采集**的节点。
 * 为什么必须探测：开发板上 /dev/video0 往往是 PxP / M2M 这类非采集节点，
 * USB 摄像头插上后落在 video1+，写死 video0 的表现就是「人脸页没画面」。 */
static int open_camera_dev(char * path_out, size_t path_cap)
{
    const char * dev = getenv("SAFE_CAMERA_DEV");
    if (dev && *dev) {
        snprintf(path_out, path_cap, "%s", dev);
        return open(path_out, O_RDWR | O_NONBLOCK);
    }

    for (int i = 0; i < 16; i++) {
        char p[CAM_DEV_PATH_MAX];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;                       /* 节点不存在，换下一个 */

        struct v4l2_capability cap;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { close(fd); continue; }
        if (!is_capture_dev(&cap)) {
            printf("[CAMERA] 跳过 %s（%s，非采集设备 cap=0x%x）\n",
                   p, cap.card, cap.capabilities);
            close(fd);
            continue;
        }
        snprintf(path_out, path_cap, "%s", p);
        return fd;
    }
    path_out[0] = '\0';
    return -1;
}

static safe_err_t v4l2_init(void)
{
    /* 打开放在 init：fd 生命周期 = init..deinit；start/stop 只控流。
     * 设备不存在不视为致命——camera_service 会降级到 null 后端。 */
    if (s_fd >= 0) return SAFE_OK;

    build_tables();

    char path[CAM_DEV_PATH_MAX];
    s_fd = open_camera_dev(path, sizeof(path));
    if (s_fd < 0) {
        printf("[CAMERA] 未找到支持采集的摄像头（可用 SAFE_CAMERA_DEV 指定）\n");
        return SAFE_ERR_FAIL;
    }

    struct v4l2_capability cap;
    if (xioctl(s_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        printf("[CAMERA] %s QUERYCAP 失败\n", path);
        close(s_fd); s_fd = -1;
        return SAFE_ERR_FAIL;
    }
    if (!is_capture_dev(&cap)) {          /* 仅在显式指定了一个非采集节点时走到 */
        printf("[CAMERA] %s 不支持采集流（cap=0x%x）\n", path, cap.capabilities);
        close(s_fd); s_fd = -1;
        return SAFE_ERR_UNSUP;
    }
    printf("[CAMERA] 选中 %s: %s\n", path, cap.card);
    return SAFE_OK;
}

static safe_err_t v4l2_deinit(void)
{
    if (s_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s_fd, VIDIOC_STREAMOFF, &type);
        s_streaming = false;
    }
    for (int i = 0; i < s_buf_count; i++) {
        if (s_bufs[i].start) munmap(s_bufs[i].start, s_bufs[i].length);
        s_bufs[i].start  = NULL;
        s_bufs[i].length = 0;
    }
    s_buf_count = 0;
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    free(s_out); s_out = NULL; s_out_size = 0;
    s_held = false;
    return SAFE_OK;
}

static safe_err_t v4l2_start(uint16_t w, uint16_t h)
{
    if (s_fd < 0) return SAFE_ERR_STATE;
    if (s_streaming) return SAFE_OK;

    /* ---- 1. 协商格式：YUYV，期望 320×240（1:1 直通，FR-16）---- */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w ? w : 320;
    fmt.fmt.pix.height      = h ? h : 240;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(s_fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[CAMERA] S_FMT 失败\n");
        return SAFE_ERR_UNSUP;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        char fourcc[5] = {0};
        memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
        printf("[CAMERA] 驱动未接受 YUYV（实际 %s），按计划停下回报，不硬解\n", fourcc);
        return SAFE_ERR_UNSUP;
    }
    s_cap_w = (uint16_t)fmt.fmt.pix.width;
    s_cap_h = (uint16_t)fmt.fmt.pix.height;
    printf("[CAMERA] 格式协商: YUYV %ux%u (bytesperline=%u)\n",
           s_cap_w, s_cap_h, fmt.fmt.pix.bytesperline);

    /* ---- 2. 请求帧率 30fps，驱动按能力收敛 ---- */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    xioctl(s_fd, VIDIOC_S_PARM, &parm);   /* 失败不阻塞：帧率尽力而为 */
    if (parm.parm.capture.timeperframe.denominator)
        printf("[CAMERA] 帧率协商: %u/%u fps\n",
               parm.parm.capture.timeperframe.numerator,
               parm.parm.capture.timeperframe.denominator);

    /* ---- 3. 申请驱动缓冲并 mmap（零拷贝）---- */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = CAM_BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        printf("[CAMERA] REQBUFS 失败\n");
        return SAFE_ERR_FAIL;
    }
    s_buf_count = (int)req.count;
    for (int i = 0; i < s_buf_count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = (unsigned)i;
        if (xioctl(s_fd, VIDIOC_QUERYBUF, &b) < 0) return SAFE_ERR_FAIL;
        s_bufs[i].length = b.length;
        s_bufs[i].start  = mmap(NULL, b.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                s_fd, b.m.offset);
        if (s_bufs[i].start == MAP_FAILED) return SAFE_ERR_FAIL;
    }

    /* ---- 4. 转换输出缓冲（320×240×2 = 150KB）---- */
    s_out_size = (size_t)s_cap_w * s_cap_h * 2;
    free(s_out);
    s_out = malloc(s_out_size);
    if (s_out == NULL) return SAFE_ERR_NOMEM;
    s_held = false;
    s_seq  = 0;

    /* ---- 5. 入队 + STREAMON ---- */
    for (int i = 0; i < s_buf_count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = (unsigned)i;
        if (xioctl(s_fd, VIDIOC_QBUF, &b) < 0) return SAFE_ERR_FAIL;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(s_fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[CAMERA] STREAMON 失败\n");
        return SAFE_ERR_FAIL;
    }
    s_streaming = true;
    printf("[CAMERA] 开始采集: %ux%u YUYV, %d buffers\n", s_cap_w, s_cap_h, s_buf_count);
    return SAFE_OK;
}

static safe_err_t v4l2_stop(void)
{
    if (!s_streaming) return SAFE_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(s_fd, VIDIOC_STREAMOFF, &type);
    s_streaming = false;
    s_held = false;
    return SAFE_OK;
}

/* YUYV 宏像素（4 字节 = 水平两像素）→ 两个 RGB565 */
static void yuyv_row_to_565(const uint8_t * src, uint16_t * dst, int pixels)
{
    for (int x = 0; x + 1 < pixels; x += 2) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        src += 4;
        int yr0 = s_y_tab[y0], yr1 = s_y_tab[y1];
        int vr  = s_v_r_tab[v];
        int ug  = s_u_g_tab[u] + s_v_g_tab[v];
        int ub  = s_u_b_tab[u];
        dst[x]     = pack565(yr0 + vr, yr0 + ug, yr0 + ub);
        dst[x + 1] = pack565(yr1 + vr, yr1 + ug, yr1 + ub);
    }
}

/* 取一帧 RGB565。无新帧（EAGAIN）返回 SAFE_ERR_BUSY；
 * 上一帧未 release 返回 SAFE_ERR_BUSY（§5.13 契约：漏调 release 后端停更）。
 *
 * 只转最新帧（规约 §3.4 / b2）：把驱动队列里已就绪的缓冲全部取出，除最后一帧外
 * 一律直接 QBUF 还回（不转换），只对最后一帧做 YUYV->RGB565——避免为已过期的帧做
 * 无用转换而拖慢串口响应。转换只在 face 线程被调用（步骤 3b 起）。 */
static safe_err_t v4l2_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    if (!s_streaming || s_out == NULL) return SAFE_ERR_STATE;
    if (s_held) return SAFE_ERR_BUSY;

    struct v4l2_buffer latest;
    memset(&latest, 0, sizeof(latest));
    bool got = false;
    for (;;) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(s_fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) break;               /* 队列已取空 */
            printf("[CAMERA] DQBUF 失败: %s\n", strerror(errno));
            return SAFE_ERR_FAIL;
        }
        if (got) xioctl(s_fd, VIDIOC_QBUF, &latest);  /* 旧帧直接还回，不转换 */
        latest = b;
        got    = true;
    }
    if (!got) return SAFE_ERR_BUSY;                   /* 摄像头还没出下一帧 */

    /* 只对最新一帧做转换：逐行处理，行内按宏像素展开（无浮点、无除法） */
    const uint8_t * src = (const uint8_t *)s_bufs[latest.index].start;
    uint16_t * dst = (uint16_t *)s_out;
    for (uint32_t y = 0; y < s_cap_h; y++) {
        yuyv_row_to_565(src + (size_t)y * s_cap_w * 2,
                        dst + (size_t)y * s_cap_w,
                        (int)s_cap_w);
    }

    xioctl(s_fd, VIDIOC_QBUF, &latest);   /* 立即还回驱动，转完即还 */

    if (rgb565) *rgb565 = s_out;
    if (info) {
        info->width     = s_cap_w;
        info->height    = s_cap_h;
        info->seq       = ++s_seq;
        info->timestamp = hal_time();
    }
    s_held = true;
    return SAFE_OK;
}

static void v4l2_release(void)
{
    s_held = false;
}

/* 采集 fd：供 face 线程 poll() 复用（§5.13）。未打开时返回 -1。 */
static int v4l2_fd(void)
{
    return s_fd;
}

const hal_camera_backend_t hal_camera_backend_v4l2 = {
    "v4l2",
    v4l2_init,
    v4l2_deinit,
    v4l2_start,
    v4l2_stop,
    v4l2_frame,
    v4l2_release,
    v4l2_fd,
};
