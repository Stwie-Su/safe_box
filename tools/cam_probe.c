/**
 * @file cam_probe.c
 * 摄像头硬件能力探测器（自研，tools/ 里的「先问硬件、再写代码」工具）。
 *
 * 【为什么要有它】本项目预览链路曾因「不知道硬件到底支持什么」而写了一大片兜底分支
 *   （YUYV/MJPG 双路径协商、各种降级回退）——那些都是硬件不确定性留下的疤痕。
 *   本工具把「猜」变成「量」：开机跑一次，输出这份设备的能力清单，代码按清单写死，
 *   兜底分支就可以有理有据地删掉。
 *
 * 【与 v4l2-ctl 的关系】`v4l2-ctl --all --list-formats-ext` 也能查这些，本工具的意义是：
 *   1) 输出格式贴合本项目（能力位逐位解释 + 帧率按 fps 折算 + 实测帧率）；
 *   2) 可直接交叉编译上板，板子上没有 v4l2-ctl；
 *   3) 本身是 V4L2 ioctl 用法的完整示例（QUERYCAP / ENUM_FMT / ENUM_FRAMESIZES /
 *      ENUM_FRAMEINTERVALS / S_FMT / S_PARM / REQBUFS / mmap / QBUF / DQBUF / STREAMON）。
 *
 * 【实测基准（2026-10-14，VM + 板上同型 FM225 UVC，Serial 2023101000）】
 *   像素格式：仅 MJPG（无 YUYV）
 *   分辨率：800x480 / 864x480 / 854x480 / 480x320 / 320x240
 *   帧间隔：全部离散单值 0.067s = 15.000 fps
 *   另：同一设备的 video1 是 Metadata Capture 节点（无 VIDEO_CAPTURE），
 *       会被本工具的设备判定正确排除——这正是 camera_v4l2.c 必须多节点扫描的原因。
 *
 * 前提：Linux + <linux/videodev2.h>；只依赖 libc，无第三方库。
 * 编译：cc -O2 -Wall -o cam_probe tools/cam_probe.c
 * 交叉：arm-buildroot-linux-gnueabihf-gcc -O2 -o cam_probe tools/cam_probe.c
 * 用法：
 *   ./cam_probe                          # 扫 /dev/video0..15，只列能力
 *   ./cam_probe -d /dev/video0           # 指定节点
 *   ./cam_probe -d /dev/video0 -b        # 列能力 + 实测帧率（默认 MJPG 320x240 取 30 帧）
 *   ./cam_probe -d /dev/video0 -b -f MJPG -w 800 -h 480 -n 60
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define CAM_PROBE_DEVS   16
#define CAM_PROBE_BUFS   4

/* ------------------------------------------------------------------
 * 函数：xioctl
 * 功能：ioctl 健壮包装，仅在被信号打断（EINTR）时重试。
 * 调用者：本文件所有 ioctl 调用点。
 * 数据流：(fd, 请求码, 参数结构体) → ioctl() → 0 成功 / -1 失败（errno 见 man）。
 * 为什么只重试 EINTR：EINTR 表示调用被信号打断、实际未执行，重试安全；
 *   EINVAL/EBUSY 等是真实结果，重试只会掩盖问题。
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
 * 函数：fourcc_str
 * 功能：把 32 位像素格式码转成可读的 4 字符（如 'MJPG'）。
 * 调用者：print_caps / list_formats。
 * 数据流：入参 fourcc → 出参 out[5]（4 字符 + '\0'，非打印字符替换为 '.'）。
 * ------------------------------------------------------------------ */
static void fourcc_str(uint32_t f, char out[5])
{
    out[0] = (char)(f & 0xFF);
    out[1] = (char)((f >> 8) & 0xFF);
    out[2] = (char)((f >> 16) & 0xFF);
    out[3] = (char)((f >> 24) & 0xFF);
    for (int i = 0; i < 4; i++)
        if (out[i] < 32 || out[i] > 126) out[i] = '.';
    out[4] = '\0';
}

/* ------------------------------------------------------------------
 * 函数：parse_fourcc
 * 功能：把 "MJPG" 这样的 4 字符串转成 V4L2 的 32 位格式码（与 v4l2_fourcc 宏同序）。
 * 调用者：main 的 -f 参数解析。
 * 数据流：入参 4 字符串 → 返回 uint32_t；长度不是 4 返回 0（调用方据此报错）。
 * ------------------------------------------------------------------ */
static uint32_t parse_fourcc(const char * s)
{
    if (s == NULL || strlen(s) != 4) return 0;
    return (uint32_t)(unsigned char)s[0]
         | ((uint32_t)(unsigned char)s[1] << 8)
         | ((uint32_t)(unsigned char)s[2] << 16)
         | ((uint32_t)(unsigned char)s[3] << 24);
}

/* ------------------------------------------------------------------
 * 函数：is_capture_dev
 * 功能：判定节点是否为「可采集 + 支持流式」的摄像头。
 * 调用者：open_dev（扫描每个节点时）、main。
 * 数据流：入参 QUERYCAP 结果 → 取能力位 → 返回 true/false。
 * ★ 与 camera_v4l2.c 的 is_capture_dev 保持**完全一致**的判定规则：
 *   驱动置了 V4L2_CAP_DEVICE_CAPS 时必须看 device_caps，否则看 capabilities。
 *   规则不一致会导致「探测说能用、实际后端扫不到」的割裂。
 * ------------------------------------------------------------------ */
static bool is_capture_dev(const struct v4l2_capability * cap)
{
    uint32_t caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? cap->device_caps : cap->capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

/* ------------------------------------------------------------------
 * 函数：open_dev
 * 功能：打开指定节点，或扫描 /dev/video0..15 找第一个采集节点。
 * 调用者：main。
 * 数据流：入参 want（NULL=扫描）→ 返回 fd，并把实际路径写进 path_out；失败返回 -1。
 * ------------------------------------------------------------------ */
static int open_dev(const char * want, char * path_out, size_t path_cap)
{
    if (want != NULL && *want) {
        snprintf(path_out, path_cap, "%s", want);
        return open(path_out, O_RDWR | O_NONBLOCK);
    }
    for (int i = 0; i < CAM_PROBE_DEVS; i++) {
        char p[64];
        struct v4l2_capability cap;
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        int  qr = xioctl(fd, VIDIOC_QUERYCAP, &cap);
        bool ok = (qr == 0) && is_capture_dev(&cap);
        if (!ok) {
            printf("  [跳过] %s（%s）\n", p, qr < 0 ? "QUERYCAP 失败" : "非采集设备");
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
 * 函数：print_caps
 * 功能：打印 QUERYCAP 拿到的设备身份与能力位（逐位解释，方便直接抄进规约）。
 * 调用者：main。
 * 数据流：入参 QUERYCAP 结果 + 节点路径 → 仅打印，无出参。
 * ------------------------------------------------------------------ */
static void print_caps(const char * path, const struct v4l2_capability * cap)
{
    printf("设备节点        : %s\n", path);
    printf("驱动            : %s\n", cap->driver);
    printf("板卡            : %s\n", cap->card);
    printf("总线            : %s\n", cap->bus_info);
    printf("驱动版本        : %u.%u.%u\n",
           (cap->version >> 16) & 0xFF, (cap->version >> 8) & 0xFF, cap->version & 0xFF);

    uint32_t caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? cap->device_caps : cap->capabilities;
    printf("能力位          : 0x%08x（%s）\n", caps,
           (cap->capabilities & V4L2_CAP_DEVICE_CAPS) ? "取自 device_caps" : "取自 capabilities");
    printf("    %s VIDEO_CAPTURE  可采集\n",   (caps & V4L2_CAP_VIDEO_CAPTURE)  ? "[有]" : "[无]");
    printf("    %s STREAMING      支持流式\n", (caps & V4L2_CAP_STREAMING)      ? "[有]" : "[无]");
    printf("    %s EXT_PIX_FORMAT 扩展格式\n", (caps & V4L2_CAP_EXT_PIX_FORMAT) ? "[有]" : "[无]");
    printf("    %s READWRITE      read/write 方式\n", (caps & V4L2_CAP_READWRITE) ? "[有]" : "[无]");
    printf("    %s ASYNCIO        异步 IO\n",  (caps & V4L2_CAP_ASYNCIO)        ? "[有]" : "[无]");
}

/* ------------------------------------------------------------------
 * 函数：print_interval
 * 功能：把一个帧间隔折算成 fps 打印（离散值直接算，区间算上下界）。
 * 调用者：list_formats。
 * 数据流：入参 v4l2_fract（分子/分母，单位秒）→ 打印 fps，无出参。
 * ------------------------------------------------------------------ */
static void print_interval(const struct v4l2_fract * iv)
{
    if (iv->numerator == 0) {
        printf("间隔非法(分子为0)");
        return;
    }
    printf("%.3f fps", (double)iv->denominator / (double)iv->numerator);
}

/* ------------------------------------------------------------------
 * 函数：list_formats
 * 功能：枚举设备支持的全部像素格式 × 分辨率 × 帧率。
 * 调用者：main。
 * 数据流：ENUM_FMT(index++) 直到驱动返回 EINVAL 表示列完；对每个格式再
 *         ENUM_FRAMESIZES，离散尺寸再 ENUM_FRAMEINTERVALS。仅打印。
 * 注意：两种枚举结构支持的形态**不一样**（写错成员名编译不过，别凭印象）：
 *   v4l2_frmsizeenum → Discrete / Stepwise 两种（**没有 continuous**）
 *   v4l2_frmivalenum → Discrete / Stepwise / Continuous 三种
 * ------------------------------------------------------------------ */
static void list_formats(int fd)
{
    printf("\n支持的采集格式（VIDIOC_ENUM_FMT 逐项枚举）：\n");
    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int nfmt = 0;
    while (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        char f[5];
        fourcc_str(fmt.pixelformat, f);
        printf("  [%d] '%s' %s%s\n", nfmt, f, fmt.description,
               (fmt.flags & V4L2_FMT_FLAG_COMPRESSED) ? "（压缩格式，需软解）" : "");
        nfmt++;

        struct v4l2_frmsizeenum fs;
        memset(&fs, 0, sizeof(fs));
        fs.pixel_format = fmt.pixelformat;
        fs.index = 0;
        while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                printf("      尺寸 %ux%u\n", fs.discrete.width, fs.discrete.height);
                struct v4l2_frmivalenum fi;
                memset(&fi, 0, sizeof(fi));
                fi.pixel_format = fmt.pixelformat;
                fi.width = fs.discrete.width;
                fi.height = fs.discrete.height;
                fi.index = 0;
                while (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0) {
                    printf("        帧率 ");
                    if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                        print_interval(&fi.discrete);
                    } else if (fi.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                        printf("stepwise %.3f ~ %.3f fps",
                               (double)fi.stepwise.max.denominator / (double)fi.stepwise.max.numerator,
                               (double)fi.stepwise.min.denominator / (double)fi.stepwise.min.numerator);
                    } else {
                        printf("continuous（任意）");
                    }
                    printf("\n");
                    fi.index++;
                }
            } else if (fs.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                printf("      尺寸 stepwise %ux%u ~ %ux%u（步进 %ux%u）\n",
                       fs.stepwise.min_width, fs.stepwise.min_height,
                       fs.stepwise.max_width, fs.stepwise.max_height,
                       fs.stepwise.step_width, fs.stepwise.step_height);
            } else {
                /* v4l2_frmsizeenum 只有 discrete / stepwise 两种形态
                 * （continuous 是 v4l2_frmivalenum 独有的），此处仅作兜底。 */
                printf("      尺寸 其它类型(type=%u)\n", fs.type);
            }
            fs.index++;
        }
        fmt.index++;
    }
    if (nfmt == 0) printf("  （驱动未报告任何采集格式）\n");
}

/* ------------------------------------------------------------------
 * 函数：bench
 * 功能：实测——按指定格式开流，取 want 帧，统计实际帧率与帧字节数。
 * 调用者：main（带 -b 时）。
 * 数据流：
 *   S_FMT(请求) → 驱动可能改分辨率，回读实际值 → S_PARM 请求 30fps 看驱动收敛
 *   → REQBUFS(4) → QUERYBUF + mmap → 全部 QBUF → STREAMON
 *   → poll(2s 超时) + DQBUF 循环，累计 bytesused 与单调时钟
 *   → STREAMOFF + munmap + close（在 main 里关 fd）
 * 为什么用 poll 而不是裸 DQBUF：fd 是非阻塞的，没帧时 DQBUF 返回 EAGAIN；
 *   poll 能让「等帧」这段时间不占 CPU，也给整体加超时上限，防止设备无输出时死等。
 * ------------------------------------------------------------------ */
static int bench(int fd, uint32_t fourcc, uint16_t w, uint16_t h, int want)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w;
    fmt.fmt.pix.height      = h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[bench] S_FMT 失败: %s\n", strerror(errno));
        return -1;
    }
    char f[5];
    fourcc_str(fmt.fmt.pix.pixelformat, f);
    printf("\n实测（请求 '%s' %ux%u）\n", f, w, h);
    printf("  驱动协商结果    : '%s' %ux%u（bytesperline=%u, sizeimage=%u）\n",
           f, fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
    if (fmt.fmt.pix.pixelformat != fourcc)
        printf("  ⚠ 驱动改写了像素格式：请求 0x%08x，实得 0x%08x\n", fourcc, fmt.fmt.pix.pixelformat);

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.denominator) {
        printf("  帧率请求        : 30 fps → 驱动收敛到 %.3f fps\n",
               (double)parm.parm.capture.timeperframe.denominator /
               (double)parm.parm.capture.timeperframe.numerator);
    } else {
        printf("  帧率请求        : 驱动不支持 S_PARM（尽力而为）\n");
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = CAM_PROBE_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        printf("[bench] REQBUFS 失败: %s\n", strerror(errno));
        return -1;
    }
    printf("  驱动缓冲        : 请求 %d 个，实得 %u 个\n", CAM_PROBE_BUFS, req.count);

    void  * starts[CAM_PROBE_BUFS];
    size_t  lengths[CAM_PROBE_BUFS];
    int     nb = (int)req.count;
    for (int i = 0; i < nb; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = (unsigned)i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("[bench] QUERYBUF[%d] 失败: %s\n", i, strerror(errno));
            return -1;
        }
        starts[i]  = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        lengths[i] = b.length;
        if (starts[i] == MAP_FAILED) {
            printf("[bench] mmap[%d] 失败: %s\n", i, strerror(errno));
            return -1;
        }
    }
    for (int i = 0; i < nb; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = (unsigned)i;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("[bench] QBUF[%d] 失败: %s\n", i, strerror(errno));
            return -1;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[bench] STREAMON 失败: %s\n", strerror(errno));
        return -1;
    }

    int      got = 0;
    uint32_t maxb = 0;
    uint64_t sumb = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (got < want) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 2000);
        if (pr < 0) {
            printf("  ⚠ poll 出错: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) {
            printf("  ⚠ poll 2s 超时：设备没有出新帧\n");
            break;
        }
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            printf("  ⚠ DQBUF 失败: %s\n", strerror(errno));
            break;
        }
        sumb += b.bytesused;
        if (b.bytesused > maxb) maxb = b.bytesused;
        got++;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("  ⚠ QBUF 失败: %s\n", strerror(errno));
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < nb; i++)
        if (starts[i] != NULL && starts[i] != MAP_FAILED)
            munmap(starts[i], lengths[i]);

    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("  取到帧数        : %d / %d\n", got, want);
    if (got > 0) {
        printf("  实测帧率        : %.3f fps（耗时 %.3f s）\n", got / sec, sec);
        printf("  帧字节          : 平均 %llu，最大 %u\n",
               (unsigned long long)(sumb / (uint64_t)got), maxb);
        printf("  → 预览带宽估算  : %.1f KB/s\n", (double)(sumb / (uint64_t)got) * (got / sec) / 1024.0);
    }
    return got > 0 ? 0 : -1;
}

int main(int argc, char ** argv)
{
    const char * dev = NULL;
    uint32_t fourcc = V4L2_PIX_FMT_MJPEG;
    uint16_t w = 320, h = 240;
    int want = 30;
    bool do_bench = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            dev = argv[++i];
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            fourcc = parse_fourcc(argv[++i]);
            if (fourcc == 0) {
                fprintf(stderr, "错误：-f 需要 4 字符格式码，如 MJPG / YUYV\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            w = (uint16_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") && i + 1 < argc) {
            h = (uint16_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            want = atoi(argv[++i]);
            if (want < 1) want = 1;
        } else if (!strcmp(argv[i], "-b")) {
            do_bench = true;
        } else if (!strcmp(argv[i], "--help")) {
            printf("用法: %s [-d /dev/videoN] [-b] [-f MJPG] [-w 320] [-h 240] [-n 30]\n", argv[0]);
            printf("  -d  指定设备节点（缺省扫 /dev/video0..15）\n");
            printf("  -b  列完能力后实测帧率\n");
            printf("  -f  像素格式四字符码（缺省 MJPG）\n");
            printf("  -w/-h  请求分辨率（缺省 320x240）  -n  实测取帧数（缺省 30）\n");
            return 0;
        } else {
            fprintf(stderr, "未知参数：%s（--help 看用法）\n", argv[i]);
            return 2;
        }
    }

    char path[64];
    int fd = open_dev(dev, path, sizeof(path));
    if (fd < 0) {
        fprintf(stderr, "找不到可用的采集设备（可试 -d /dev/videoN）\n");
        return 1;
    }

    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "QUERYCAP 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("===== 摄像头能力清单（cam_probe）=====\n");
    print_caps(path, &cap);
    list_formats(fd);

    if (do_bench) {
        if (bench(fd, fourcc, w, h, want) != 0)
            fprintf(stderr, "[bench] 实测失败\n");
    } else {
        printf("\n（加 -b 可实测帧率，例如 -b -f MJPG -w 320 -h 240 -n 30）\n");
    }

    printf("=====================================\n");
    close(fd);
    return 0;
}
