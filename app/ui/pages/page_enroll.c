/**
 * @file page_enroll.c
 * 人脸录入页（FR-19/FR-27 细化）：录入专用全屏窗口，与「人脸识别」页分开
 *（用户拍板 2026-09-15：录入只管录入，识别只管识别）。
 *
 * 结构（root 纵向 flex）：
 *   1) 头部单行：标题「人脸录入」+ 引导副文案（模组 NOTE 实时状态）
 *   2) 画面区：视频预览面板（占满）+ 引导小字徽章（叠在视频顶部居中）
 *   3) 底部行：状态文案 + 「取消录入」（回用户页；离开本页即 duty-cycle 终止会话）
 *
 * 生命周期：用户页发起录入 → 切入本页；ENROLL_DONE（成功/失败/30s 兜底超时）
 * 由 page_users 的 on_face_event_ui 统一处理并自动切回用户页。
 *
 * 帧管线与 page_face 同构（拉帧 → 旋转 → 最近邻缩放 → 查表绘制）；两页互斥
 * 可见，face_thread 双帧缓冲任一时刻只有一个消费者。
 * TODO(架构债)：预览管线与 page_face 重复，后续抽 ui/preview 共用组件。
 */
#include "ui/pages/page_enroll.h"
#include "ui/pages/page_users.h"     /* page_users_retry_enroll：失败态「重新录入」 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/icons.h"
#include "ui/ui_feedback.h"
#include "ui/ui.h"
#include "hal/hal_face.h"
#include "hal/hal_camera.h"
#include "hal/face/face_thread.h"
#include "hal/hal_time.h"

/* 预览帧管线共用常量（CAP_W/CAP_H/PREVIEW_MS/FACE_FRAME_STALE_MS/FACE_MAX_SCALE）：
 * 与 page_face 人脸识别页**共用同一份默认值**，避免两页各抄一份、漏初值而分叉。 */
#include "ui/pages/preview_cfg.h"

/* ===== 静态句柄 ===== */
static lv_obj_t * s_root;
static lv_obj_t * s_sub;              /* 头部引导副文案（模组 NOTE 实时状态） */
static lv_obj_t * s_preview;
static lv_obj_t * s_canvas;
static lv_obj_t * s_guide_lb;         /* 录入实时引导小字（叠在视频顶部居中） */
static lv_obj_t * s_box_obj;          /* 人脸框（模组 NOTE 上报的框，叠加在预览上） */
static bool       s_box_visible;      /* 框当前是否显示（避免每 500ms 重复 set 引发重绘） */
static lv_obj_t * s_status;           /* 底部状态文案 */
static lv_timer_t * s_frame_timer;
static lv_timer_t * s_status_timer;

/* ===== 帧管线（与 page_face 同构；TODO 抽共用组件） ===== */
static uint16_t * s_canvas_buf;
static int32_t    s_canvas_w, s_canvas_h;
static int32_t    s_vid_x, s_vid_y, s_vid_w, s_vid_h;
static uint16_t * s_map_x, * s_map_y;
static uint16_t   s_cap_buf[CAP_W * CAP_H];
static int        s_rot_deg = FACE_PREVIEW_ROT_DEFAULT;   /* 默认值见 preview_cfg.h */
static int32_t    s_scale_max = FACE_MAX_SCALE;   /* 放大上限默认值必须与 page_face 同源，否则 0 → 视频矩形夹成 1×1 空白 */
static int32_t    s_src_w = CAP_W, s_src_h = CAP_H;
static uint16_t * s_rot_buf;
static bool       s_got_frame;
static uint32_t   s_last_frame_ms;

/* 录入结果态（用户拍板 2026-09-15：录入失败**不要弹 modal**，在引导窗口内显示）。
 * s_result_shown=true 期间 s_status 由结果文案独占，状态定时器不再覆盖（否则
 * 500ms 后就被冲掉）；底部按钮同时切成「重新录入」。 */
static bool       s_result_shown;
static bool       s_result_ok;
static lv_obj_t * s_result_btn_lb;    /* 底部按钮文字：「取消录入」/「重新录入」 */

static void frame_timer_cb(lv_timer_t * t);
static void status_timer_cb(lv_timer_t * t);
static void cancel_cb(lv_event_t * e);
static void preview_env_init(void);
static void rotate_frame(const uint16_t * src, uint16_t * dst,
                         int32_t sw, int32_t sh, int deg);
static void compute_video_rect(int32_t cw, int32_t ch);
static void rebuild_maps(void);
static void rebuild_canvas(void);
static const char * guide_text(int32_t fs, bool five_way);
static void refresh_foot_btn(void);

/* ===== env 开关（与 page_face 共用同一组 env；板上不设即走默认） ===== */
static void preview_env_init(void)
{
    const char * er = getenv("SAFE_CAMERA_ROT");
    if (er != NULL) {
        int v = atoi(er);
        if (v == 0 || v == 90 || v == 180 || v == 270) s_rot_deg = v;   /* 显式 0 也受理 */
    }
    const char * es = getenv("SAFE_FACE_PREVIEW_SCALE");
    if (es != NULL) {
        int32_t v = (int32_t)atoi(es);
        if (v >= 100 && v <= 5000) s_scale_max = v;
    }
    s_src_w = (s_rot_deg == 90 || s_rot_deg == 270) ? CAP_H : CAP_W;
    s_src_h = (s_rot_deg == 90 || s_rot_deg == 270) ? CAP_W : CAP_H;

    /* 旋转缓冲随「需要的旋转角度」生命周期管理（消除体检项：分配后无 free）：
     *   - 重复分配前先 free 并置 NULL → 页面重建/重复调用不泄漏、不复用陈旧缓冲；
     *   - rot==0 时不需要旋转缓冲，直接释放，尽量少占内存（板上仅 512MB）。
     * 时序安全：本函数与 frame_timer_cb 同在 LVGL 主线程串行执行，且帧回调以
     * (s_rot_deg != 0 && s_rot_buf != NULL) 双重判定，释放后绝不会被读到。 */
    free(s_rot_buf);
    s_rot_buf = NULL;
    if (s_rot_deg != 0) {
        s_rot_buf = (uint16_t *)malloc(sizeof(uint16_t) * CAP_W * CAP_H);
        if (s_rot_buf == NULL) {      /* 分配失败：放弃旋转，保持原方向显示 */
            s_rot_deg = 0;
            s_src_w   = CAP_W;
            s_src_h   = CAP_H;
        }
    }
}

/* 把 sw×sh 的 RGB565 帧旋转 deg 后写入 dst（90 = 顺时针） */
static void rotate_frame(const uint16_t * src, uint16_t * dst, int32_t sw, int32_t sh, int deg)
{
    if (deg == 90) {
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)x * sh + (sh - 1 - y)] = src[(size_t)y * sw + x];
    } else if (deg == 180) {
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)(sh - 1 - y) * sw + (sw - 1 - x)] = src[(size_t)y * sw + x];
    } else {   /* 270 */
        for (int32_t y = 0; y < sh; y++)
            for (int32_t x = 0; x < sw; x++)
                dst[(size_t)(sw - 1 - x) * sh + y] = src[(size_t)y * sw + x];
    }
}

/* 计算 4:3（或旋转后 3:4）视频矩形：等比缩放取最大，居中不拉伸 */
static void compute_video_rect(int32_t cw, int32_t ch)
{
    int32_t sx = (int32_t)(((int64_t)cw * 1000) / s_src_w);
    int32_t sy = (int32_t)(((int64_t)ch * 1000) / s_src_h);
    int32_t scale = (sx < sy) ? sx : sy;
    if (scale < 1) scale = 1;
    /* 放大封顶按 UI 缩放系数同步放大（上限语义 = 设计空间 1024×600 的倍数） */
    if (scale > (int32_t)(s_scale_max * ui_scale_x()))
        scale = (int32_t)(s_scale_max * ui_scale_x());

    s_vid_w = (int32_t)(((int64_t)s_src_w * scale) / 1000);
    s_vid_h = (int32_t)(((int64_t)s_src_h * scale) / 1000);
    if (s_vid_w < 1) s_vid_w = 1;
    if (s_vid_h < 1) s_vid_h = 1;
    if (s_vid_w > cw) s_vid_w = cw;
    if (s_vid_h > ch) s_vid_h = ch;
    s_vid_x = (cw - s_vid_w) / 2;
    s_vid_y = (ch - s_vid_h) / 2;
    if (s_vid_x < 0) s_vid_x = 0;
    if (s_vid_y < 0) s_vid_y = 0;
}

static void rebuild_maps(void)
{
    free(s_map_x);
    free(s_map_y);
    s_map_x = NULL;
    s_map_y = NULL;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    s_map_x = (uint16_t *)malloc((size_t)s_vid_w * sizeof(uint16_t));
    s_map_y = (uint16_t *)malloc((size_t)s_vid_h * sizeof(uint16_t));
    if (s_map_x == NULL || s_map_y == NULL) {
        free(s_map_x); free(s_map_y);
        s_map_x = NULL; s_map_y = NULL;
        return;
    }
    for (int32_t x = 0; x < s_vid_w; x++) {
        int32_t v = (int32_t)(((int64_t)x * s_src_w) / s_vid_w);
        if (v >= s_src_w) v = s_src_w - 1;
        s_map_x[x] = (uint16_t)v;
    }
    for (int32_t y = 0; y < s_vid_h; y++) {
        int32_t v = (int32_t)(((int64_t)y * s_src_h) / s_vid_h);
        if (v >= s_src_h) v = s_src_h - 1;
        s_map_y[y] = (uint16_t)v;
    }
}

/* 画布缓冲：尺寸 = 视频矩形（同 page_face 的 T3 结论，留白零开销） */
static void rebuild_canvas(void)
{
    int32_t cw = s_vid_w;
    int32_t ch = s_vid_h;
    if (cw <= 0 || ch <= 0) return;

    if (cw == s_canvas_w && ch == s_canvas_h && s_canvas_buf != NULL) {
        if (s_canvas) {
            lv_obj_set_size(s_canvas, cw, ch);
            lv_obj_set_pos(s_canvas, s_vid_x, s_vid_y);
        }
        return;
    }

    size_t bytes = (size_t)cw * (size_t)ch * 2u;
    uint16_t * nb = (uint16_t *)malloc(bytes);
    if (nb == NULL) return;
    uint16_t bg565 = lv_color_to_u16(theme_color(TH_PANEL));
    for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++) nb[i] = bg565;

    if (s_canvas) {
        lv_canvas_set_buffer(s_canvas, nb, cw, ch, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_canvas, cw, ch);
        lv_obj_set_pos(s_canvas, s_vid_x, s_vid_y);
    }
    free(s_canvas_buf);
    s_canvas_buf = nb;
    s_canvas_w   = cw;
    s_canvas_h   = ch;
}

/* NOTE state → 引导文案（FR-19；语义见 hal_face.h face_service_face_state）
 *
 * fs < 0 = 模组**还没上报过**任何 FACE_STATE：backend_fm225.c 里 s_face_state
 * 初值就是 -1，而模组只在录入/验证会话开始后才上报 NOTE，会话刚发起到第一帧
 * 之间 state 恒为 -1。原先 -1 落进 default，被当成「未检测到人脸」——模组还没
 * 开口界面就先报「没检测到人脸」，用户据此反复怀疑「为什么总是识别不到」。
 * 这里给中性文案，与「已开口但没看到脸」（fs=1 等）区分开。 */
static const char * guide_text(int32_t fs, bool five_way)
{
    if (fs < 0) {
        return five_way ? "请正对镜头，准备开始五向录入…"
                        : "正在等待模组响应，请正对模组…";
    }
    switch (fs) {
        /* 五向模式下模组要依次采集 正/右/左/下/上（face_direction 低 5 位）。
         *
         * ★ 真机实测（2026-09-20）：模组是**一次性**回 SUCCESS + 掩码 0x1F 的
         *   （914ms、会话期间仅 1 条 NOTE），**不提供中间进度** —— 所以做不了
         *   「已完成 3/5」这类追踪式提示（那是模组的限制，不是 UI 没做）。
         *   能做的、也最有效的，就是把**动作序列**写清楚：
         *   实测第一次录入因动作不符**一直等到 12s 会话超时**才失败，
         *   而动作正确时 1s 内就完成 —— 文案直接决定成败。
         *
         * 沿用单帧那句「请保持不动」则必然失败：用户不动，模组采不到其它朝向。 */
        case 0:  return five_way
                             ? "五向录入：请依次 正对 → 右转 → 左转 → 低头 → 抬头"
                             : "已检测到人脸，请保持不动";
        case 2:  return "人脸太靠上，请下移一点";
        case 3:  return "人脸太靠下，请上移一点";
        case 4:  return "人脸太靠左，请右移一点";
        case 5:  return "人脸太靠右，请左移一点";
        default: return "未检测到人脸，请正对模组";
    }
}

/* 五向：某一位是否已完成 → 已完成的步数（用于「第 N/5 步」） */
static int mask_popcount(uint8_t v)
{
    int n = 0;
    while (v) { n += (v & 1); v >>= 1; }
    return n;
}

/* 五向：下一个待采集方向 → 人话动作（位序同 fm225 的方向表） */
static const char * dir_word(uint8_t d)
{
    switch (d) {
        case 0x01: return "正对镜头";
        case 0x02: return "向右转头";
        case 0x04: return "向左转头";
        case 0x08: return "请低头";
        case 0x10: return "请抬头";
        default:   return "保持不动";
    }
}

/* 五向分步引导：**在视频上方**依次告诉用户当前该做哪个方向。
 * 优先级：模组报出的位置类问题（太靠上/下/左/右）> 当前该做的方向。
 * —— 人都不在画面里时，先让人把脸摆对，再谈转头。 */
static void five_way_guide(char * buf, size_t n, uint8_t mask, uint8_t next_d,
                          int32_t fs)
{
    const int done = mask_popcount(mask);
    if (next_d == 0) {
        snprintf(buf, n, "五向录入：五个方向已采齐，处理中…");
        return;
    }
    if (fs >= 2 && fs <= 5) {
        snprintf(buf, n, "第 %d/5 步：%s", done + 1, guide_text(fs, true));
        return;
    }
    snprintf(buf, n, "第 %d/5 步：%s", done + 1, dir_word(next_d));
}

/* ===== 拉帧定时器（本页可见才拉帧；两页互斥 → 单消费者） ===== */
static void frame_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_root == NULL || lv_obj_is_hidden(s_root)) return;
    if (!s_canvas || s_canvas_w <= 0 || s_canvas_h <= 0) return;
    if (s_vid_w <= 0 || s_vid_h <= 0) return;

    hal_camera_frame_info_t info;
    if (face_thread_get_preview(s_cap_buf, &info)) {
        const uint16_t * src = s_cap_buf;
        if (s_rot_deg != 0 && s_rot_buf != NULL) {
            rotate_frame(s_cap_buf, s_rot_buf, CAP_W, CAP_H, s_rot_deg);
            src = s_rot_buf;
        }

        int32_t dst_stride = s_canvas_w;
        uint16_t * buf = s_canvas_buf;
        for (int32_t y = 0; y < s_canvas_h; y++) {
            uint16_t * dst_row = &buf[(size_t)y * dst_stride];
            if (s_map_x && s_map_y) {
                const uint16_t * src_row = &src[(size_t)s_map_y[y] * s_src_w];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    dst_row[x] = src_row[s_map_x[x]];
                }
            } else {
                int32_t sy = (int32_t)(((int64_t)y * s_src_h) / s_canvas_h);
                const uint16_t * src_row = &src[sy * s_src_w];
                for (int32_t x = 0; x < s_canvas_w; x++) {
                    int32_t sx = (int32_t)(((int64_t)x * s_src_w) / s_canvas_w);
                    dst_row[x] = src_row[sx];
                }
            }
        }

        s_got_frame = true;
        s_last_frame_ms = hal_time_ms();
        lv_obj_invalidate(s_canvas);
    }
}

/* ===== 人脸框（叠加在预览上） =====
 * 数据源：模组 NOTE 帧的 v[1..4] = left/top/right/bottom（经 face_service_face_box 读出）。
 *
 * ★ 坐标映射链与预览帧管线**完全同源**：
 *   模组坐标(按 CAP_W×CAP_H) → 按预览旋转角变换 → ×缩放系数 → +视频矩形偏移。
 *   ⚠️ 注意：模组的检测相机与 app 预览用的 UVC 相机**不是同一个传感器**，
 *   两者的视场/安装位置不同 —— 所以这个框是「模组视角」的投影，位置/大小
 *   只能作参考。若显示明显错位，开 SAFE_FM225_NOTE_DEBUG 看框数值范围再标定。 */
static void face_box_update(void)
{
    if (s_box_obj == NULL) return;
    int16_t l = 0, t = 0, r = 0, b = 0;
    if (face_service_face_box(&l, &t, &r, &b) != 0 || (l | t | r | b) == 0) {
        if (s_box_visible) { lv_obj_set_hidden(s_box_obj, true); s_box_visible = false; }
        return;
    }
    if (s_vid_w <= 0 || s_vid_h <= 0) {
        if (s_box_visible) { lv_obj_set_hidden(s_box_obj, true); s_box_visible = false; }
        return;
    }
    /* ★ 先乘后除（int64 中间量）：整数除法先算 kx=vid_w/src_w 会**丢掉全部小数**，
     *   视频矩形比源小的时候 kx 直接得 0，框宽高随之归零、根本画不出来。 */
    /* ★ 映射基准 = 模组检测图 480×640（竖版 3:4，实测标定）：
     *   三条样本框中心 x=(L+R)/2 除以 480 恒为 49~50%（完美居中），
     *   而 320×240 会出界（R=356>320）、640×480 中心 x=37% 偏左 —— 唯一自洽。
     * 且 480×640 与预览旋转后的竖版源（240×320）同视场（1.5 倍分辨率），
     * 方向一致 → 直接按比例映射，不做旋转。若后续发现上下镜像，在 scale 前翻 y。 */
    const int32_t x1 = s_vid_x + (int32_t)((int64_t)l * s_vid_w / 480);
    const int32_t y1 = s_vid_y + (int32_t)((int64_t)t * s_vid_h / 640);
    const int32_t x2 = s_vid_x + (int32_t)(((int64_t)r + 1) * s_vid_w / 480);
    const int32_t y2 = s_vid_y + (int32_t)(((int64_t)b + 1) * s_vid_h / 640);
    if (x2 <= x1 || y2 <= y1 || x2 < 0 || y2 < 0 || x1 > s_canvas_w || y1 > s_canvas_h) {
        if (s_box_visible) { lv_obj_set_hidden(s_box_obj, true); s_box_visible = false; }
        return;
    }
    lv_obj_set_pos(s_box_obj, x1, y1);
    lv_obj_set_size(s_box_obj, x2 - x1, y2 - y1);
    if (!s_box_visible) { lv_obj_set_hidden(s_box_obj, false); s_box_visible = true; }
}

/* ===== 状态定时器（500ms）：引导小字 + 状态文案 ===== */
static void status_timer_cb(lv_timer_t * t)
{
    (void)t;
    if (s_root == NULL || lv_obj_is_hidden(s_root)) return;

    uint32_t now = hal_time_ms();
    bool live = s_got_frame && ((now - s_last_frame_ms) < FACE_FRAME_STALE_MS);
    bool enrolling = face_service_enrolling();
    /* 录入模式（单帧 / 五向）决定引导文案；后端无此概念时为 false（单帧）。
     * 姿态 yaw/pitch/roll 已由 face_service_face_pose() 提供，暂未参与文案
     * —— 转头/抬头的判定阈值需要真机标定，标定前只用 state 更稳妥。 */
    bool five_way = face_service_enroll_five_way();

    /* 引导小字：仅录入会话期间显示在视频上（FR-19）。
     * fs < 0（模组尚未上报）不是「没检测到人脸」，由 guide_text 统一给中性文案。 */
    if (enrolling) {
        int32_t fs = face_service_face_state();
        if (five_way) {
            char g[96];
            five_way_guide(g, sizeof(g),
                           face_service_enroll_dir_mask(),
                           face_service_enroll_next_dir(), fs);
            lv_label_set_text(s_guide_lb, g);
        } else {
            lv_label_set_text(s_guide_lb, guide_text(fs, false));
        }
        lv_obj_set_hidden(s_guide_lb, false);
    } else {
        lv_obj_set_hidden(s_guide_lb, true);
    }

    face_box_update();   /* 人脸框随模组 NOTE 更新（500ms 节拍足够） */

    /* 头部副文案（冗余一份引导，低视觉权重） */
    /* 录入期间把模组实时上报的**姿态**显示出来：
     * ①便于观察模组到底在不在看脸；②为后续「转到位」的阈值标定提供依据
     *   （yaw/pitch 的正负与量程尚未标定，先只显示数值，不据此判定）。 */
    if (enrolling) {
        int16_t yaw = 0, pitch = 0, roll = 0;
        char sb[96];
        /* 同验证页：把模组实时上报的**全部**信息都摆出来（状态 + 人脸框 + 姿态）。 */
        int16_t l = 0, t = 0, rr = 0, bb = 0;
        char boxs[40] = "";
        if (face_service_face_box(&l, &t, &rr, &bb) == 0 && (l | t | rr | bb))
            snprintf(boxs, sizeof(boxs), " 框(%d,%d,%d,%d)", l, t, rr, bb);
        if (face_service_face_pose(&yaw, &pitch, &roll) == 0) {
            snprintf(sb, sizeof(sb), "状态=%d%s  姿态 yaw=%d pitch=%d roll=%d",
                     (int)face_service_face_state(), boxs,
                     (int)yaw, (int)pitch, (int)roll);
        } else {
            snprintf(sb, sizeof(sb), "状态=%d%s（模组未上报姿态）",
                     (int)face_service_face_state(), boxs);
        }
        lv_label_set_text(s_sub, sb);
    } else {
        lv_label_set_text(s_sub, five_way ? "未在录入会话 · 五向模式"
                                          : "未在录入会话");
    }

    /* 底部状态文案：明确、不空。
     * 结果态（录入成功/失败文案）由 page_enroll_show_result 写入并独占 s_status，
     * 这里不覆盖 —— 否则 500ms 后结果就被冲掉了。 */
    if (!s_result_shown) {
        const char * st;
        if (!face_service_enrolling())     st = "未在录入会话 · 从「用户」页发起录入";
        else if (!live)                    st = "等待摄像头出帧…";
        else                               st = "录入会话进行中 · 完成后自动返回用户页";
        lv_label_set_text(s_status, st);
    }
}

/* 底部按钮语义：失败态 = 「重新录入」（就地重试），其余 = 「取消录入」（回用户页） */
static void refresh_foot_btn(void)
{
    if (s_result_btn_lb == NULL) return;
    lv_label_set_text(s_result_btn_lb,
                      (s_result_shown && !s_result_ok) ? "重新录入" : "取消录入");
}

/* 录入结果在页内展示（page_users 的 on_face_event_ui 调用）。
 * text=NULL 表示清除结果态，把 s_status 交回状态定时器托管。 */
void page_enroll_show_result(const char * text, bool ok)
{
    if (s_root == NULL || s_status == NULL) return;

    if (text == NULL) {
        s_result_shown = false;
        s_result_ok    = false;
        lv_obj_set_style_text_color(s_status, theme_color(TH_TEXT_MUT), 0);
        refresh_foot_btn();
        return;
    }

    s_result_shown = true;
    s_result_ok    = ok;
    lv_label_set_text(s_status, text);
    /* 成功用成功色、失败用告警色 —— 一律走 theme token，禁止硬编码颜色 */
    lv_obj_set_style_text_color(s_status, theme_color(ok ? TH_OK : TH_WARN), 0);
    refresh_foot_btn();
}

static void cancel_cb(lv_event_t * e)
{
    (void)e;
    /* 失败态：就地重新发起录入（用户拍板：不弹窗、不切页，让人对着引导重试） */
    if (s_result_shown && !s_result_ok) {
        if (page_users_retry_enroll()) {
            page_enroll_show_result(NULL, false);   /* 清结果态，交回状态定时器 */
            return;
        }
        /* 重发没被受理（模组忙等）：保留失败态并换一句，让用户再点一次 */
        page_enroll_show_result("重新录入发起失败，请稍后重试", false);
        return;
    }
    /* 不需要显式 abort：离开本页 → duty-cycle 判定前台离开 → 0x10 RESET 终止在途会话 */
    ui_switch_page(PAGE_USERS);
}

/* ===== SIZE_CHANGED：重算视频矩形 / 映射表 / 画布 / 引导小字位置 ===== */
static void preview_size_changed_cb(lv_event_t * e)
{
    (void)e;
    if (s_preview == NULL) return;
    int32_t w = lv_obj_get_width(s_preview);
    int32_t h = lv_obj_get_height(s_preview);
    if (w <= 0 || h <= 0) return;

    compute_video_rect(w, h);
    rebuild_maps();
    rebuild_canvas();
    if (s_guide_lb) {
        lv_obj_align(s_guide_lb, LV_ALIGN_TOP_MID, 0, s_vid_y + SY(8));
    }
}

/* 页面根对象被 LVGL 删除时的释放点：回收帧管线全部堆缓冲（旋转/画布/映射表）
 * 并清空句柄，使页面重建时重新走分配路径（不泄漏、不复用陈旧缓冲）。
 *
 * 时序安全（本处 free 不会引入 UAF，逐条论证）：
 *   1) 本回调由根对象 LV_EVENT_DELETE 触发，位于 LVGL obj_delete_core() 的
 *      「递归删除子对象」**之前**；从 free 发生到画布真正析构之间，全程为
 *      单线程同步路径（主线程，lv_timer_handler 不会穿插），其间不发生任何
 *      渲染或建帧，因此不会有代码再读这些缓冲。
 *   2) 画布析构（lv_canvas_destructor → lv_image_destructor）只调用
 *      lv_image_cache_drop() 按 src 指针摘除解码缓存，**不解引用像素数据、
 *      也不 free 用户缓冲**（缓冲所有权在本模块），故不存在二次释放。
 *   3) 末尾置空 s_root：frame_timer_cb / status_timer_cb 均以 s_root==NULL 早退，
 *      页面销毁后不再访问任何帧缓冲，同时消除悬挂指针。
 * 结论：在此处回收 s_canvas_buf/s_map_x/s_map_y 是安全的（QA 已确认无现行 UAF），
 * 修复「页面销毁→重建」路径上的堆泄漏。 */
static void page_enroll_deleted_cb(lv_event_t * e)
{
    (void)e;
    free(s_rot_buf);
    s_rot_buf = NULL;
    free(s_canvas_buf);
    s_canvas_buf = NULL;
    free(s_map_x);
    s_map_x = NULL;
    free(s_map_y);
    s_map_y = NULL;
    s_canvas_w = 0;
    s_canvas_h = 0;
    s_root    = NULL;
}

lv_obj_t * page_enroll_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    s_root = root;
    /* 页面销毁释放点：回收旋转缓冲并清空句柄，使页面重建时重新走分配路径。 */
    lv_obj_add_event_cb(root, page_enroll_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_outline_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, SX(8), 0);
    lv_obj_set_style_pad_row(root, SY(4), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(root, false);

    /* ---------- 头部单行：标题 + 实时引导副文案 ---------- */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_outline_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, SX(12), 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "人脸录入");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(18), 0);

    s_sub = lv_label_create(head);
    lv_label_set_text(s_sub, "");
    lv_obj_add_style(s_sub, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_sub, app_font_scaled(12), 0);
    lv_obj_set_flex_grow(s_sub, 1);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_DOT);

    /* ---------- 画面区：视频预览面板（flex grow 占满） ---------- */
    s_preview = lv_obj_create(root);
    lv_obj_set_size(s_preview, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_preview, 1);
    lv_obj_set_style_min_width(s_preview, SY(240), 0);
    lv_obj_set_style_min_height(s_preview, SY(180), 0);
    lv_obj_add_style(s_preview, &st_panel, 0);
    lv_obj_set_style_radius(s_preview, SX(16), 0);
    lv_obj_set_style_bg_color(s_preview, theme_color(TH_PANEL), 0);
    lv_obj_set_style_border_width(s_preview, 1, 0);
    lv_obj_set_style_border_color(s_preview, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(s_preview, 0, 0);
    lv_obj_set_style_pad_all(s_preview, 0, 0);
    lv_obj_set_scroll_dir(s_preview, LV_DIR_NONE);
    lv_obj_set_scrollable(s_preview, false);
    lv_obj_add_event_cb(s_preview, preview_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    /* 人脸框：绿色 2px 边框的透明矩形，浮在画布上层；坐标由 face_box_update 维护 */
    s_box_obj = lv_obj_create(s_preview);
    lv_obj_remove_style_all(s_box_obj);
    lv_obj_set_style_border_color(s_box_obj, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_border_width(s_box_obj, 2, 0);
    lv_obj_set_style_border_opa(s_box_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_box_obj, 2, 0);
    lv_obj_set_hidden(s_box_obj, true);
    s_box_visible = false;

    s_canvas = lv_canvas_create(s_preview);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);
    lv_canvas_fill_bg(s_canvas, theme_color(TH_PANEL), LV_OPA_COVER);

    /* 引导小字（叠在视频顶部居中；位置在 SIZE_CHANGED 里随视频矩形更新） */
    s_guide_lb = lv_label_create(s_preview);
    lv_label_set_text(s_guide_lb, "");
    lv_obj_set_style_bg_color(s_guide_lb, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_guide_lb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_guide_lb, SX(8), 0);
    lv_obj_set_style_pad_all(s_guide_lb, SX(8), 0);
    lv_obj_set_style_text_font(s_guide_lb, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(s_guide_lb, theme_color(TH_TEXT), 0);
    lv_obj_set_hidden(s_guide_lb, true);

    /* ---------- 底部行：状态 + 取消 ---------- */
    lv_obj_t * foot = lv_obj_create(root);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_outline_width(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_set_style_pad_column(foot, SX(12), 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(foot, false);

    s_status = lv_label_create(foot);
    lv_label_set_text(s_status, "");
    lv_obj_add_style(s_status, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_status, app_font_scaled(12), 0);
    lv_obj_set_flex_grow(s_status, 1);

    lv_obj_t * cancel = lv_button_create(foot);
    lv_obj_set_size(cancel, SX(128), SY(38));
    lv_obj_set_style_radius(cancel, SX(12), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(cancel, 0, 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cancel, SX(8), 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消录入");
    lv_obj_set_style_text_font(cl, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(cl, theme_color(TH_TEXT), 0);
    lv_obj_center(cl);
    s_result_btn_lb = cl;        /* 失败态由 refresh_foot_btn 改成「重新录入」 */

    /* ---------- 定时器 ---------- */
    if (s_frame_timer == NULL) {
        s_frame_timer = lv_timer_create(frame_timer_cb, PREVIEW_MS, NULL);
        s_status_timer = lv_timer_create(status_timer_cb, 500, NULL);
    }

    preview_env_init();
    lv_obj_update_layout(root);
    preview_size_changed_cb(NULL);

    return root;
}

bool page_enroll_is_visible(void)
{
    return (s_root != NULL) && !lv_obj_is_hidden(s_root);
}
