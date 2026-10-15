/**
 * @file camera_v4l2.c
 * V4L2 摄像头后端（Sprint3 步骤 2，FR-16 视频预览）。
 *
 * 设计要点（对应规约 §4.2 性能预算 / §5.13 相机接口约定）：
 *  1. V4L2 mmap 零拷贝采集：驱动缓冲直接映射，DQBUF 后在用户态做
 *     MJPG→RGB565 软解（stb_image，只转最新帧，坏帧丢弃不停流），再 QBUF 还回驱动。
 *     ★ 2026-10-14 起**锁定 MJPG 单格式**：经 tools/cam_probe.c 实测，目标设备
 *       （FM225 的 UVC，Card = Fuge_UVC_Interface）只报告 MJPG 一种像素格式，
 *       原「优先 YUYV、失败降级 MJPG」的双路径协商中 YUYV 分支**永远走不到**，
 *       连同 5 张 YUV 查表与 yuyv_row_to_565() 一并删除（详见文件末「删除记录」）。
 *  2. 采集 = 显示 = 320×240（需求 v1.6 定案，1:1 直通无缩放开销）。
 *     start(w,h) 传期望值，驱动协商结果以 S_FMT 返回为准；
 *  3. 帧缓冲归后端所有（§5.13 接口约定）：frame() 返回内部转换缓冲，
 *     消费方必须调 release()，否则下一帧返回 SAFE_ERR_BUSY（后端停更）；
 *  4. 本步（步骤 2）转换发生在调用方线程（LVGL 主循环 20ms tick）；
 *     步骤 3 引入 face 线程后由 face 线程 poll 该 fd（§3.4）。
 *
 * 设备：自动探测 /dev/video0..15 中第一个支持采集 + 流式的节点（板子上 video0 常是 PxP 这类
 *       非采集节点，真摄像头落在 video1+）；可用环境变量 SAFE_CAMERA_DEV 显式覆盖。
 * 采集帧率：S_PARM 请求 30fps，驱动按能力收敛——实测该 UVC **固定 15fps**
 *        （cam_probe: 请求 30 → 收敛 15，320×240 实测 17.8fps，480×320 实测 12.4fps）。
 *
 * ==================================================================
 * 【阅读地图】本文件是自底向上的第 1 层（最贴硬件），建议按下表顺序读
 * ==================================================================
 *  ① 状态全景      → 下面「文件级状态一览表」（先记住有哪些静态量）
 *  ② 打开设备      → open_camera_dev()  / is_capture_dev()  / xioctl()
 *  ③ 开机与收尾    → v4l2_init() / v4l2_deinit() / unmap_bufs()
 *  ④ 开流（重头戏）→ v4l2_start()：协商格式 → 申请缓冲 mmap → 入队 → STREAMON
 *  ⑤ 取帧（重头戏）→ v4l2_frame()：DQBUF 排空队列 → 只转最新一帧 → QBUF 还回
 *  ⑥ 像素转换      → v4l2_frame() 内的 MJPG 软解（stb_image）+ pack565() 打包
 *  ⑦ 出口          → 文件末尾 hal_camera_backend_v4l2（把自己登记成 vtable 供上层取用）
 *
 * 【V4L2 采集的通用套路】（换任何 Linux 摄像头都适用，这 8 步就是考点）
 *   open → QUERYCAP → S_FMT → S_PARM → REQBUFS → QUERYBUF+mmap → QBUF → STREAMON
 *   然后循环：DQBUF（取出填满的缓冲）→ 处理 → QBUF（还回驱动）……
 *   结束：STREAMOFF → munmap → close
 *   ★ 关键心智模型：缓冲是**驱动和用户的共享队列**，用户用完必须还回去，
 *     队列空了就没帧了；因此「忘记 QBUF」= 慢慢把队列掏空 → 预览最终定格。
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

/* MJPG 解码（决-1，2026-09-14）：FM225 的 UVC 只出 MJPG（实测无 YUYV 档），
 * 预览走 320×240@15fps 小图软解。stb_image 单头文件、无外部依赖，PC/ARM 同源；
 * 实现实例化在 third_party/stb/stb_image_impl.c（safe_thirdparty，-w 不污染
 * 全仓告警目标），本文件只引入声明。 */
#include <stb_image.h>

#define CAM_BUF_COUNT   4
#define CAM_DEV_PATH_MAX 64

/* 一个 mmap 驱动缓冲的用户态视图：起始地址 + 长度 */
typedef struct {
    void  * start;
    size_t  length;
} v4l2_buf_t;

/* ==================================================================
 * 【文件级状态一览表】本文件是**单实例**设计：状态全部是文件内 static，
 *   所以同一进程只允许有一个 V4L2 后端（与「一台设备一个预览」的需求相符）。
 *
 *   名字         含义                          谁写                谁读
 *   -----------  --------------------------    -----------------   --------------
 *   s_fd         设备 fd（/dev/videoX）        init 打开 / deinit   frame / fd
 *   s_streaming  是否已 STREAMON               start / stop         frame（前置判断）
 *   s_bufs[]     mmap 后的驱动缓冲（4 个）     start 映射           frame（读像素）
 *   s_buf_count  实际映射成功的缓冲个数        start 填             start / unmap
 *   s_cap_w/h    协商后的真实分辨率            start 填             frame（转换用）
 *   s_seq        帧序号（单调递增）            frame 每次 ++        frame（填 info）
 *   s_out        RGB565 输出缓冲（后端所有）   start 分配           frame 写入后交出
 *   s_out_size   s_out 字节数 = w*h*2          start 计算           —
 *   s_held       上一帧是否取走未归还          frame 置 true        frame（防重入）
 *
 * ⚠️ 并发约定（读代码前务必先接受这条前提）：
 *   本后端**自身不加锁**。它依赖上层的时序约定——
 *     · 生命周期类（init / deinit / start / stop）由**主线程**调用（app.c、face_service.c）；
 *     · 取帧类（frame / release）由 **face 线程**调用，且被 face_thread.c 的 io 锁包住。
 *   即「start/stop」与「frame」分属两个线程而**无共同锁**，两者重叠时的安全性由
 *   face_service 的 duty-cycle 时序（先离前台停流、再进前台开流）保证。
 *   改动任一处时序前，请先回到 face_thread.c 核对 io 锁的覆盖范围。
 * ================================================================== */

static int        s_fd = -1;
static bool       s_streaming;
static v4l2_buf_t s_bufs[CAM_BUF_COUNT];
static int        s_buf_count;

/* 协商后的采集格式 */
static uint16_t   s_cap_w, s_cap_h;
static uint32_t   s_seq;

/* RGB565 输出缓冲（后端所有，frame() 交给消费方） */
static uint8_t  * s_out;
static size_t     s_out_size;
static bool       s_held;      /* 消费方未 release 时为 true */

/* ------------------------------------------------------------------
 * 函数：pack565（文件内私有）
 * 功能：把 R/G/B 三个 0..255 的分量打包成一个 RGB565 半字（把 8bit 截成 5/6/5bit）。
 * 调用者：v4l2_frame() 的 MJPG 分支——每个像素都要调一次（320×240 = 7.68 万次/帧）。
 * 数据流：(r,g,b) 三个 int → 先夹紧到 [0,255] → 位拼装 → 返回 uint16_t。
 * 为什么先夹紧：stb_image 解出的 RGB888 本应在 0..255，但万一越界，
 *   不夹紧会出现「亮的地方发黑 / 暗的地方发白」的回卷（wrap）伪影。
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * 函数：xioctl（文件内私有）
 * 功能：ioctl 的健壮包装——只在被信号打断（EINTR）时重试，其它错误原样返回。
 * 调用者：本文件所有 ioctl 调用点（QUERYCAP / S_FMT / S_PARM / REQBUFS /
 *         QUERYBUF / QBUF / DQBUF / STREAMON / STREAMOFF）。**本文件不直接调 ioctl**。
 * 数据流：(fd, 请求码, 参数结构体指针) → ioctl() → 返回 0 成功 / -1 失败（错误码在 errno）。
 * 为什么只重试 EINTR：EINTR 表示「系统调用被信号打断，其实什么都没做」，重试安全；
 *   而 EINVAL / EBUSY / ENODEV 都是**真实结果**，重试只会掩盖问题、还可能是死循环。
 *   这是 Linux 系统编程的标准写法（man 手册里反复强调的 EINTR 处理）。
 * ------------------------------------------------------------------ */
static int xioctl(int fd, unsigned long req, void * arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ------------------------------------------------------------------
 * 函数：is_capture_dev（文件内私有）
 * 功能：判断一个设备节点是不是「能往外吐图像」的采集设备。
 * 调用者：open_camera_dev()（扫描时每个节点问一次）、v4l2_init()（校验显式指定的节点）。
 * 数据流：入参 QUERYCAP 得到的 v4l2_capability → 取能力位 → 返回 true/false。
 * 判定条件（两个都满足才算）：
 *   V4L2_CAP_VIDEO_CAPTURE —— 能采集（而不是只能接收输出的 M2M 处理节点）
 *   V4L2_CAP_STREAMING     —— 支持流式（mmap / DMABUF 方式；只有 read() 的不算）
 * ★ 坑（V4L2 的约定，不是本项目的）：现代驱动会置 V4L2_CAP_DEVICE_CAPS 标志，
 *   此时**必须读 device_caps 而不是 capabilities**——后者是「整个物理设备」的能力
 *   并集，会把同一设备的其它功能也算进来，导致误判。板子上的 PxP 是 M2M 图像处理
 *   节点，只有 VIDEO_OUTPUT 无 VIDEO_CAPTURE，靠这条规则被正确排除。
 * ------------------------------------------------------------------ */
static bool is_capture_dev(const struct v4l2_capability * cap)
{
    uint32_t caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? cap->device_caps : cap->capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}


/* ------------------------------------------------------------------
 * 函数：open_camera_dev（文件内私有）
 * 功能：找到并打开一个可用的采集设备节点，把节点路径回写给调用方。
 * 调用者：v4l2_init()（唯一调用点）。
 * 数据流：
 *   ① 环境变量 SAFE_CAMERA_DEV 有值 → 直接开这个（调试/多摄像头时指定用）；
 *   ② 否则遍历 /dev/video0..15，逐个 open → QUERYCAP → is_capture_dev 过滤，
 *      取第一个通过的；
 *   ③ 出参 path_out 收到实际使用的路径（日志打印用），返回 fd，失败返回 -1。
 * 为什么不用 O_RDONLY：V4L2 采集虽然主要读数据，但 ioctl（S_FMT 等）需要读写权限，
 *   惯例一律用 O_RDWR。
 * 为什么用 O_NONBLOCK：配套 DQBUF 时「没帧可读」会立即返回 EAGAIN 而不是阻塞，
 *   这样 face 线程可以靠 poll + 非阻塞读做到「不空转也不卡死」。
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * 函数：v4l2_init
 * 功能：后端初始化——打开设备节点 + 确认它是采集设备。**不开流**。
 * 调用者：camera_service.c 的 hal_camera_init()（经 vtable 转发）。
 * 数据流：无入参；副作用 = 置 s_fd（成功）或保持 -1（失败）。
 * 设计取舍：fd 的生命周期 = init..deinit；start/stop 只负责「开流 / 停流」。
 *   这样反复进出人脸页（duty-cycle）不会反复 open/close，减少 USB 重枚举风险。
 * 失败语义：返回 SAFE_ERR_FAIL（没节点）/ SAFE_ERR_UNSUP（节点不是采集设备），
 *   上层 camera_service.c 收到非 OK 会降级到 null 后端。
 * ------------------------------------------------------------------ */
static safe_err_t v4l2_init(void)
{
    /* 打开放在 init：fd 生命周期 = init..deinit；start/stop 只控流。 */
    if (s_fd >= 0) return SAFE_OK;

    char path[CAM_DEV_PATH_MAX];
    /* 选节点并打开（扫描 /dev/video0..15 或按 SAFE_CAMERA_DEV） */
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

/* 释放全部已 mmap 的驱动缓冲并复位计数（幂等）。
 * 用途有二：(1) deinit 正常收尾；(2) start 中途失败（QUERYBUF / mmap 失败）的清理——
 * 后者若不清理，会把**前序已映射**的缓冲泄漏（真机表现为反复进退人脸页后映射数耗尽、
 * 进程 RSS 缓慢上涨，最终 V4L2 取不到缓冲、预览全黑）。 */
/* ------------------------------------------------------------------
 * 函数：unmap_bufs（文件内私有）
 * 功能：解除全部已 mmap 的驱动缓冲映射并复位计数（幂等，可重复调用）。
 * 调用者：v4l2_deinit()（正常收尾）、v4l2_start()（两条失败清理路径：
 *         QUERYBUF 失败 / mmap 失败 / 快速恢复失败后的完整重开）。
 * 数据流：无入参出参；逐个 munmap(s_bufs[i]) → 指针置 NULL、长度置 0 → s_buf_count=0。
 * ------------------------------------------------------------------ */
static void unmap_bufs(void)
{
    for (int i = 0; i < s_buf_count; i++) {
        if (s_bufs[i].start != NULL && s_bufs[i].start != MAP_FAILED)
            munmap(s_bufs[i].start, s_bufs[i].length);
        s_bufs[i].start  = NULL;
        s_bufs[i].length = 0;
    }
    s_buf_count = 0;
}

/* ------------------------------------------------------------------
 * 函数：v4l2_deinit
 * 功能：后端收尾——停流 → 解映射 → 释放输出缓冲 → 关 fd。**全部资源归零**。
 * 调用者：camera_service.c 的 hal_camera_deinit()（关机路径）。
 * 数据流：无入参出参；释放 s_bufs[] / s_out / s_fd，并复位 s_streaming / s_held。
 * ★ 调用顺序要求（与 face_thread.c 的退出序列绑定）：
 *     face_thread_request_stop()（STREAMOFF + 等在途采集退出）
 *       → hal_camera_stop()（幂等）
 *       → 本函数（close fd）
 *       → face_thread_join()
 *   若在 face 线程还在 DQBUF 时 close(fd)，线程可能卡在内核里或读到已释放的映射。
 * ------------------------------------------------------------------ */
static safe_err_t v4l2_deinit(void)
{
    if (s_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s_fd, VIDIOC_STREAMOFF, &type);
        s_streaming = false;
    }
    unmap_bufs();
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    free(s_out); s_out = NULL; s_out_size = 0;
    s_held = false;
    return SAFE_OK;
}

/* ------------------------------------------------------------------
 * 函数：v4l2_start ★本文件最核心的函数之一
 * 功能：开始采集——协商格式与帧率 → 申请并 mmap 驱动缓冲 → 全部入队 → STREAMON。
 * 调用者：camera_service.c 的 hal_camera_start()（开机 / 人脸页进前台）。
 * 数据流：
 *   入参 (w,h) 期望分辨率
 *     → S_FMT(MJPG) → 真实分辨率写入 s_cap_w/h
 *     → S_PARM 请求 30fps（尽力而为，失败不阻塞）
 *     → REQBUFS 向驱动要 4 个缓冲 → 逐个 QUERYBUF 查偏移 → mmap 映射到用户态
 *     → 分配 s_out（RGB565 输出缓冲，w*h*2 字节）
 *     → 4 个缓冲全部 QBUF 入队 → STREAMON 开始出帧
 *   出参：无；状态全部落在上面那张「文件级状态一览表」里。
 *
 * 两条进入路径（务必分清，改动前先判断自己改的是哪条）：
 *   A) 快速恢复（s_buf_count>0 且 s_out 已分配）：上一轮 stop 只做了 STREAMOFF，
 *      格式与 mmap 都还在，只需重新入队 + STREAMON。省掉重复的 REQBUFS/mmap。
 *   B) 完整重开（首次启动，或 A 失败后退回）：走上面那串完整流程。
 * ------------------------------------------------------------------ */
static safe_err_t v4l2_start(uint16_t w, uint16_t h)
{
    if (s_fd < 0) return SAFE_ERR_STATE;
    if (s_streaming) return SAFE_OK;

    /* ---- 0. 快速恢复路径（FR-27 duty-cycle 重启）：stop 只做了 STREAMOFF，
     * 格式协商与 mmap 缓冲都还在 —— 重新入队 + STREAMON 即可。
     * （旧实现每次 start 都重新 REQBUFS+mmap：映射泄漏，且旧映射失效，
     *   真机表现为「离开人脸页再回来，预览停格 0fps」——用户验收 2026-09-15。） ---- */
    if (s_buf_count > 0 && s_out != NULL) {
        /* 快速恢复路径（FR-27）：stop 只做 STREAMOFF，重新入队 + STREAMON。
         * 2026-10-02 真机修复：板载 FM225 的 UVC 实现（ffff:ffff，非标）在
         * STREAMOFF 后不接受直接重新入队，QBUF 静默 EINVAL，导致「离开人脸页
         * 再回来预览 0 帧」。现把 QBUF/STREAMON 失败改为**退回完整重开路径**
         *（STREAMOFF 清态 + unmap + REQBUFS(0) 释放驱动缓冲），自愈且带回 errno 日志。 */
        bool q_ok = true;
        for (int i = 0; i < s_buf_count; i++) {
            struct v4l2_buffer b;
            memset(&b, 0, sizeof(b));
            b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index  = (unsigned)i;
            if (xioctl(s_fd, VIDIOC_QBUF, &b) < 0) {
                printf("[CAMERA] 快速恢复 QBUF[%d] 失败: %s\n", i, strerror(errno));
                q_ok = false;
                break;
            }
        }
        if (q_ok) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(s_fd, VIDIOC_STREAMON, &type) < 0) {
                printf("[CAMERA] 快速恢复 STREAMON 失败: %s\n", strerror(errno));
                q_ok = false;
            }
        }
        if (q_ok) {
            s_streaming = true;
            s_held      = false;
            printf("[CAMERA] 恢复采集: %ux%u MJPG(软解), %d buffers\n", s_cap_w, s_cap_h, s_buf_count);
            return SAFE_OK;
        }

        /* 落回完整重开：清掉半开的流状态与缓冲，让下方完整路径重新协商 */
        printf("[CAMERA] 快速恢复失败，退回完整重开路径\n");
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s_fd, VIDIOC_STREAMOFF, &type);
        unmap_bufs();
        struct v4l2_requestbuffers req0;
        memset(&req0, 0, sizeof(req0));
        req0.count  = 0;
        req0.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req0.memory = V4L2_MEMORY_MMAP;
        xioctl(s_fd, VIDIOC_REQBUFS, &req0);
    }

    /* ---- 1. 协商格式：**直接请求 MJPG**（2026-10-14 起锁定单格式）----
     * 依据：tools/cam_probe.c 实测目标设备（FM225 的 UVC）只报告 MJPG 一种格式，
     *   原「先 YUYV、失败再 MJPG」的两段式协商里 YUYV 分支永远走不到，已删除。
     * 仍保留「驱动是否照做」的校验：UVC 驱动存在**静默改写 pixelformat** 的已知
     *   行为（请求 A 返回 B），不校验就会把 MJPG 缓冲当 YUYV 解、画面花屏。
     * 若将来要换只支持 YUYV 的相机：把下面 pixelformat 改成 V4L2_PIX_FMT_YUYV，
     *   并恢复 yuyv_row_to_565() 与 5 张 YUV 查表（git 历史可查，见文件末删除记录）。 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w ? w : 320;
    fmt.fmt.pix.height      = h ? h : 240;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(s_fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* 打 errno 是为了区分 EINVAL（设备真不支持 MJPG，换相机时才会遇到）与
         * EBUSY（设备被别的进程占着 / 双实例互抢）——二者在界面上都表现为
         * 「预览全黑」，必须能分辨。 */
        printf("[CAMERA] MJPG 未被接受（errno=%d, %s）\n", errno, strerror(errno));
        return SAFE_ERR_UNSUP;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        char fourcc[5] = {0};
        memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
        printf("[CAMERA] 驱动改写格式为 '%s'，本后端只支持 MJPG\n", fourcc);
        return SAFE_ERR_UNSUP;
    }
    printf("[CAMERA] 格式协商: MJPG %ux%u (sizeimage=%u)\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);
    s_cap_w = (uint16_t)fmt.fmt.pix.width;
    s_cap_h = (uint16_t)fmt.fmt.pix.height;

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
        if (xioctl(s_fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("[CAMERA] QUERYBUF[%d] 失败\n", i);
            unmap_bufs();               /* 释放前序已映射缓冲，杜绝泄漏 */
            return SAFE_ERR_FAIL;
        }
        s_bufs[i].length = b.length;
        s_bufs[i].start  = mmap(NULL, b.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                s_fd, b.m.offset);
        if (s_bufs[i].start == MAP_FAILED) {
            s_bufs[i].start = NULL;     /* 复位，避免后续 unmap 对 MAP_FAILED 误操作 */
            printf("[CAMERA] mmap[%d] 失败: %s\n", i, strerror(errno));
            unmap_bufs();               /* 中途失败：释放前序已映射缓冲（体检项修复） */
            return SAFE_ERR_FAIL;
        }
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
    printf("[CAMERA] 开始采集: %ux%u MJPG(软解), %d buffers\n", s_cap_w, s_cap_h, s_buf_count);
    return SAFE_OK;
}

/* ------------------------------------------------------------------
 * 函数：v4l2_stop
 * 功能：停止采集（STREAMOFF）。**保留** 格式协商结果、mmap 缓冲与 s_out，
 *       以便 v4l2_start() 走「快速恢复」路径。
 * 调用者：camera_service.c 的 hal_camera_stop()（离前台 / 关机）。
 * 数据流：无入参出参；副作用 = STREAMOFF + 置 s_streaming=false / s_held=false。
 * ------------------------------------------------------------------ */
static safe_err_t v4l2_stop(void)
{
    if (!s_streaming) return SAFE_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(s_fd, VIDIOC_STREAMOFF, &type);
    s_streaming = false;
    s_held = false;
    return SAFE_OK;
}

/* 取一帧 RGB565。无新帧（EAGAIN）返回 SAFE_ERR_BUSY；
 * 上一帧未 release 返回 SAFE_ERR_BUSY（§5.13 接口约定：漏调 release 后端停更）。
 *
 * 只转最新帧（规约 §3.4 / b2）：把驱动队列里已就绪的缓冲全部取出，除最后一帧外
 * 一律直接 QBUF 还回（不解码），只对最后一帧做 MJPG→RGB565——避免为已过期的帧做
 * 无用解码而拖慢串口响应。转换只在 face 线程被调用（步骤 3b 起）。 */
/* ------------------------------------------------------------------
 * 函数：v4l2_frame ★本文件最核心的函数
 * 功能：取出「最新一帧」并转成 RGB565 交给调用方（不是队列里最旧的那帧）。
 * 调用者：face_thread.c 的 face_capture_once()（经 hal_camera_frame → vtable 转发）。
 *
 * 【数据流（一张图看懂）】
 *
 *    驱动缓冲队列（4 个，ring）
 *   ┌───┐ ┌───┐ ┌───┐ ┌───┐
 *   │ A │ │ B │ │ C │ │ D │      ← STREAMON 后驱动往里填帧
 *   └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
 *     │     │     │     │
 *     └─ DQBUF 循环（本函数第 ① 步）：一直取到 EAGAIN
 *          │
 *          ├─ 非最后一帧 → 立刻 QBUF 还回驱动（**不解码**，省 CPU）
 *          └─ 最后一帧   → 留下，进入第 ② 步
 *                             │
 *   ② 转换：MJPG 软解（stb_image → RGB888 → 打包 RGB565）→ 写入 s_out
 *                             │
 *   ③ QBUF 把刚用过的缓冲还回驱动（队列不会枯竭）
 *                             │
 *   ④ 出参 rgb565 = s_out（**内部缓冲，非拷贝**），填 info{宽/高/序号/时间戳}
 *      并置 s_held = true（上锁：直到 release 前不再取新帧）
 *
 * 【为什么只转最新帧】预览只关心「现在长什么样」。若采集 20fps 而消费只有 15fps，
 *   队列里会积压旧帧，逐个转换既浪费 CPU 又让画面越来越滞后（延迟累积）。
 *   排空 + 只转最新，等于「自动丢弃过期帧」，延迟恒定为 1 帧。
 *
 * 【★ 接口约定（§5.13）】rgb565 指向后端内部缓冲 s_out：
 *   · 调用方可以在返回后立刻读，用完必须 hal_camera_release_frame()；
 *   · 未 release 期间本函数恒返回 SAFE_ERR_BUSY（后端「停更」保护，防止读到半改的缓冲）；
 *   · 因此**绝不能缓存这个指针留着以后用**——下一帧会把它覆盖掉。
 * ------------------------------------------------------------------ */
static safe_err_t v4l2_frame(const uint8_t ** rgb565, hal_camera_frame_info_t * info)
{
    if (!s_streaming || s_out == NULL) return SAFE_ERR_STATE;
    if (s_held) return SAFE_ERR_BUSY;

    /* ① 排空队列：把驱动里已就绪的缓冲全部 DQBUF 出来，只保留最后一帧 */
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
            /* QA-07：退出前必须把**已持有的 latest 归还驱动**。
             * 否则每遇到一次 DQBUF 硬错误就永久少一个驱动缓冲，几次之后队列
             * 被掏空 → 再也取不到帧，且**不可自愈**（下面的 QBUF 在循环内，
             * 直接 return 会跳过它）。 */
            if (got) xioctl(s_fd, VIDIOC_QBUF, &latest);
            return SAFE_ERR_FAIL;
        }
        if (got) xioctl(s_fd, VIDIOC_QBUF, &latest);  /* 旧帧直接还回，不转换 */
        latest = b;
        got    = true;
    }
    if (!got) return SAFE_ERR_BUSY;                   /* 摄像头还没出下一帧 */

    /* 只对最新一帧做转换（转完立即 QBUF 还回驱动）
     * MJPG：mmap 缓冲即整帧 JPEG（bytesused 为长度），stb 软解到 RGB888
     * 再打包 RGB565。坏帧/截断帧（USB 抖动）解码失败时按「本帧无效」处理：
     * 还回驱动等下一帧，不置错不退出——预览丢一帧远好于停流。 */
    const uint8_t * jpg = (const uint8_t *)s_bufs[latest.index].start;
    int jw = 0, jh = 0, comp = 0;
    unsigned char * rgb = stbi_load_from_memory(jpg, (int)latest.bytesused,
                                                &jw, &jh, &comp, 3);
    if (rgb == NULL) {
        xioctl(s_fd, VIDIOC_QBUF, &latest);
        return SAFE_ERR_BUSY;
    }
    if ((uint16_t)jw != s_cap_w || (uint16_t)jh != s_cap_h) {
        /* 尺寸不符（理论上 UVC 不会发生），丢弃并还原驱动缓冲 */
        stbi_image_free(rgb);
        xioctl(s_fd, VIDIOC_QBUF, &latest);
        return SAFE_ERR_BUSY;
    }
    uint16_t * dst565 = (uint16_t *)s_out;
    const unsigned char * px = rgb;
    const size_t npix = (size_t)s_cap_w * s_cap_h;
    for (size_t i = 0; i < npix; i++) {
        dst565[i] = pack565(px[0], px[1], px[2]);
        px += 3;
    }
    stbi_image_free(rgb);

    xioctl(s_fd, VIDIOC_QBUF, &latest);   /* 立即还回驱动 */

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

/* ------------------------------------------------------------------
 * 函数：v4l2_release
 * 功能：归还上一帧的所有权——只是把 s_held 置回 false，**不释放内存**。
 * 调用者：face_thread.c 的 face_capture_once()（与 frame() 成对，两处：正常拷贝后、
 *        分辨率不符丢弃时）。
 * 数据流：无入参出参；解锁后端，允许下一次 frame() 取新帧。
 * ★ 漏调后果：frame() 从此恒返回 SAFE_ERR_BUSY，预览**永久定格在最后一帧**，
 *   且不报任何错——这类 bug 只能靠「发现画面不动了」才察觉，排查成本很高。
 * ------------------------------------------------------------------ */
static void v4l2_release(void)
{
    s_held = false;
}

/* ------------------------------------------------------------------
 * 函数：v4l2_fd
 * 功能：把底层采集 fd 交出去，供调用方做 IO 复用（poll / select）。
 * 调用者：face_thread.c 线程主体（经 hal_camera_fd → vtable 转发），
 *        每轮 poll 前问一次。
 * 数据流：无入参；返回 s_fd。未 init / 已 deinit 时 s_fd 为 -1，
 *        poll() 会忽略负 fd（revents 恒 0），调用方无需为此写分支。
 * 为什么需要它：这是「把阻塞式采集接进事件循环」的关键——
 *   有了 fd，face 线程才能用一个 poll 同时等「相机来帧」和「串口来字节」。
 * ------------------------------------------------------------------ */
static int v4l2_fd(void)
{
    return s_fd;
}

/* ------------------------------------------------------------------
 * 本后端对外的唯一出口：把自己登记成一张 vtable。
 * 谁用它：camera_service.c 文件头 extern 声明了它，并放进 find_backend() 的注册表。
 * 为什么用 const：整张表编译期定好、放只读段，运行期不会被改，也就没有并发问题。
 * 成员顺序必须与 camera_backend.h 的 hal_camera_backend_t 定义**严格一致**
 *（C 的指定初始化若用位置写法，错位不会报错但会跑错函数——改结构体时要同步改这里）。
 * ------------------------------------------------------------------ */
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

/* ==================================================================
 * 【删除记录】2026-10-14：删除 YUYV 支持（硬件不确定性留下的疤痕）
 * ==================================================================
 * 依据（tools/cam_probe.c 实测，2026-10-14，VM 与板上同型设备）：
 *   设备：Fuge_UVC_Interface（FM225 的 UVC），Serial 2023101000
 *   格式：仅 'MJPG' 一种（VIDIOC_ENUM_FMT 只返回 index=0）
 *   帧率：全部离散单值 0.067s = 15.000 fps；请求 30fps 驱动收敛到 15
 *   320×240 实测 17.8fps、帧均 20732 字节；480×320 实测 12.4fps
 *
 * 删除内容（约 120 行，均因「不知道硬件给什么格式」而存在的兜底）：
 *   - build_tables() 与 5 张 YUV→RGB 查表（s_y_tab / s_v_r_tab / s_u_g_tab /
 *     s_v_g_tab / s_u_b_tab / s_tab_ready）
 *   - yuyv_row_to_565()（YUYV 逐行转换）
 *   - v4l2_negotiate_mjpg()（MJPG 降级协商，已内联进 v4l2_start）
 *   - s_is_mjpg 字段与 v4l2_frame() 的 if/else 双分支
 *   - v4l2_start() 的「先 YUYV、失败再 MJPG」两段式协商
 *
 * 保留内容（同样是疤痕，但**有真实需求**，删除会引入回归）：
 *   - 多节点扫描 /dev/video0..15：实测 video1 是同设备的 Metadata Capture 节点
 *     （无 VIDEO_CAPTURE），不扫描会选错节点；板子上 video0 还是 PxP M2M 节点。
 *   - 快速恢复 vs 完整重开两条路径：这颗非标 UVC（ffff:ffff）STREAMOFF 后
 *     QBUF 会静默 EINVAL，没有回退路径就会「离开人脸页再回来预览 0 帧」。
 *   - MJPG 坏帧丢弃不停流：USB 抖动会出截断帧，停流比丢帧更糟。
 *   - S_FMT 后校验驱动是否照做：UVC 驱动存在静默改写 pixelformat 的已知行为。
 *
 * 如何恢复 YUYV（换一台只支持 YUYV 的相机时）：
 *   1) v4l2_start() 里 fmt.fmt.pix.pixelformat 改成 V4L2_PIX_FMT_YUYV；
 *   2) 恢复 yuyv_row_to_565() 与 5 张查表（git 历史：2026-10-14 之前的版本）；
 *   3) v4l2_frame() 里按 bytesused 是否为整帧 JPEG 重新分出两个转换分支。
 * ================================================================== */
