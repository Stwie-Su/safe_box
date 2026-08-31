/**
 * @file backend_fm225.c
 * FM225 双目人脸识别模组后端 —— 协议层占位实现。
 *
 * 【当前状态】模组未到货，本文件只搭好骨架：状态机、帧解析入口、超时处理都已就位，
 * 缺的是 UART 收发与帧解析的具体实现（标记 TODO-FM225）。
 *
 * 【到货后要做的事】
 *  1. 向厂家索取 UART 协议文档，确认帧头、长度、命令字、CRC16 多项式与校验范围；
 *  2. 在 fm225_open() 里打开串口并按文档配置波特率（常见 115200 8N1）；
 *  3. 在 fm225_parse_frame() 里按真实帧格式解析 face_id 与 score；
 *  4. 在 fm225_cmd_enroll() / fm225_cmd_delete() 里下发注册/删除命令。
 *
 * 上述四点改完，业务层（auth_fsm、UI、RPC）一行都不用动。
 */

#include "face_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* TODO-FM225：按厂家文档确认以下常量 */
#define FM225_FRAME_HEAD0     0xEFu
#define FM225_FRAME_HEAD1     0xAAu
#define FM225_FRAME_MAX       64
#define FM225_ENROLL_TIMEOUT  15000
#define FM225_DETECT_TIMEOUT  5000

typedef enum {
    FM225_ST_IDLE = 0,
    FM225_ST_WAIT_ENROLL_ACK,
    FM225_ST_WAIT_DELETE_ACK,
} fm225_state_t;

static fm225_state_t s_state = FM225_ST_IDLE;
static bool          s_started;

/* 串口句柄：真实实现里放 fd 或平台句柄，此处仅占位 */
static int           s_uart_fd = -1;

/* ---------------- 串口收发（待实现） ---------------- */

/* TODO-FM225：打开并配置串口，成功返回 SAFE_OK */
static safe_err_t fm225_open(void)
{
    /* 参考实现：
     *   s_uart_fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
     *   tcgetattr / cfsetispeed / cfsetospeed(115200) / tcsetattr
     *   dev 由设备树与 udev 规则决定，建议从环境变量 SAFE_FACE_DEV 取，默认 /dev/ttymxc2
     */
    return SAFE_ERR_UNSUP;
}

static void fm225_close(void)
{
    s_uart_fd = -1;
}

/* TODO-FM225：按协议拼帧并下发 */
static safe_err_t fm225_send_cmd(uint8_t cmd, const uint8_t * payload, size_t len)
{
    (void)cmd; (void)payload; (void)len;
    if(s_uart_fd < 0) return SAFE_ERR_STATE;
    return SAFE_ERR_UNSUP;
}

/* TODO-FM225：从串口缓冲里找一帧完整数据并解析 */
static safe_err_t fm225_parse_frame(const uint8_t * buf, size_t len,
                                    int32_t * out_face_id, int32_t * out_score)
{
    (void)buf; (void)len;
    if(out_face_id) *out_face_id = -1;
    if(out_score)   *out_score   = 0;
    return SAFE_ERR_UNSUP;
}

/* ---------------- 后端接口实现 ---------------- */

static safe_err_t fm225_init(void)
{
    s_state   = FM225_ST_IDLE;
    s_started = false;
    return fm225_open();
}

static safe_err_t fm225_deinit(void)
{
    fm225_close();
    return SAFE_OK;
}

static safe_err_t fm225_start(void)
{
    safe_err_t e = fm225_send_cmd(0x10, NULL, 0);   /* TODO-FM225：命令字待确认 */
    if(e == SAFE_OK) s_started = true;
    return e;
}

static safe_err_t fm225_stop(void)
{
    s_started = false;
    return fm225_send_cmd(0x11, NULL, 0);           /* TODO-FM225：命令字待确认 */
}

static void fm225_tick(uint32_t now_ms)
{
    if(!s_started || s_uart_fd < 0) return;

    /* TODO-FM225：读串口缓冲 → 找帧头 → 校验 CRC16 → 解析 → 上报
     * 解析出识别结果时：
     *     face_result_t r = { .face_id = id, .score = score, .timestamp = hal_time() };
     *     face_service_emit(FACE_EV_DETECT, &r);
     * 录入/删除应答到达时：
     *     face_enroll_result_t r = { .face_id = id, .err = SAFE_OK };
     *     face_service_emit(FACE_EV_ENROLL_DONE, &r);
     */
    (void)now_ms;
    (void)fm225_parse_frame;
}

static safe_err_t fm225_enroll(void)
{
    if(s_state != FM225_ST_IDLE) return SAFE_ERR_BUSY;
    safe_err_t e = fm225_send_cmd(0x20, NULL, 0);   /* TODO-FM225：注册命令 */
    if(e == SAFE_OK) s_state = FM225_ST_WAIT_ENROLL_ACK;
    return e;
}

static safe_err_t fm225_delete_tpl(int32_t face_id)
{
    if(face_id < 0) return SAFE_ERR_PARAM;
    if(s_state != FM225_ST_IDLE) return SAFE_ERR_BUSY;
    uint8_t p[4];
    p[0] = (uint8_t)(face_id & 0xFF);
    p[1] = (uint8_t)((face_id >> 8) & 0xFF);
    p[2] = (uint8_t)((face_id >> 16) & 0xFF);
    p[3] = (uint8_t)((face_id >> 24) & 0xFF);
    safe_err_t e = fm225_send_cmd(0x21, p, sizeof(p));
    if(e == SAFE_OK) s_state = FM225_ST_WAIT_DELETE_ACK;
    return e;
}

/* 真实模组不支持注入，调用方应在调用前查 FACE_CAP_INJECT */
static safe_err_t fm225_inject(int32_t face_id, int32_t score)
{
    (void)face_id; (void)score;
    return SAFE_ERR_UNSUP;
}

static const face_backend_t backend = {
    .name          = "fm225",
    .caps          = FACE_CAP_DETECT | FACE_CAP_ENROLL | FACE_CAP_DELETE | FACE_CAP_LIVENESS,
    .max_templates = 1000,
    .init          = fm225_init,
    .deinit        = fm225_deinit,
    .start         = fm225_start,
    .stop          = fm225_stop,
    .tick          = fm225_tick,
    .enroll        = fm225_enroll,
    .delete_tpl    = fm225_delete_tpl,
    .inject        = fm225_inject,
};

const face_backend_t * face_backend_fm225(void)
{
    return &backend;
}
