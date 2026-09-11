/**
 * @file debug_hooks.c
 * 调试钩子实现。仅 PC（SDL）构建有实质内容，其它平台为空函数。
 *
 * 环境变量：
 *   SAFE_TEST_PAGE  = HOME | LOGS | SETTINGS | USERS | NETWORK | SYSTEM | KEYPAD | FACE | OTP
 *   SAFE_TEST_THEME = 0..3
 *   SAFE_TEST_DLG   = add_user | auth | change_pwd
 *   SAFE_TEST_SHOT  = 截图输出路径（原始 RGB565，截图后直接退出）
 *   SAFE_TEST_FACE  = 注入一次人脸事件后截图（UI 现代化 ui1 验收用），取值：
 *       match[<n>]       识别成功（face_id=n，默认 1）→ 「欢迎回来，{用户名}」+ 扫描框绿
 *       no_match         未匹配 ×1 → 「未匹配（1/3）」+ 扫描框红
 *       no_match3        未匹配 ×3 → 转 WAIT_OTP（FSM 自动切动态码页）
 *       liveness         活体失败 → DANGER 横幅 + 扫描框红
 *       timeout          识别超时 → INFO 横幅、扫描框不变
 *       error            模组异常 → INFO 横幅
 *       enroll_ok/enroll_fail    录入完成（成功/失败）
 *       delete_ok/delete_fail    删除完成（成功/失败）
 *       lockout          连续失败达 max_failed → LOCKOUT 倒计时常驻横幅
 *
 * 注入时机：放在 take_shot 的 3s 稳定循环**之后**——横幅是有 hold 时长的瞬时
 * 动效，若在稳定前注入，等到抓屏时它已经淡出，截图里什么都看不到。
 */

#include "platform/debug_hooks.h"

#if defined(SAFE_PLATFORM_PC) || defined(SAFE_ENABLE_DEBUG_HOOKS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "lvgl/draw/lv_snapshot.h"

#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/pages/page_monitor.h"
#include "core/auth/auth_fsm.h"
#include "core/event_bus.h"
#include "hal/hal_face.h"
#include "hal/hal_time.h"

/* 页面里的测试入口：只在调试构建使用 */
extern void page_users_test_open_add_dlg(void);
extern void page_users_test_open_auth_dlg(void);
extern void page_users_test_open_change_pwd_dlg(void);
extern void page_users_test_open_otp_dlg(void);

static ui_page_t page_from_name(const char * name)
{
    if(strcmp(name, "LOGS") == 0)     return PAGE_LOGS;
    if(strcmp(name, "SETTINGS") == 0) return PAGE_SETTINGS;
    if(strcmp(name, "USERS") == 0)    return PAGE_USERS;
    if(strcmp(name, "NETWORK") == 0)  return PAGE_NETWORK;
    if(strcmp(name, "SYSTEM") == 0)   return PAGE_SYSTEM;
    if(strcmp(name, "KEYPAD") == 0)   return PAGE_KEYPAD;
    if(strcmp(name, "OTP") == 0)      return PAGE_OTP;
    if(strcmp(name, "FACE") == 0)     return PAGE_FACE;
    return PAGE_HOME;
}

/* ---------------- 人脸事件注入（UI 现代化 ui1 截图验收，spec §7） ----------------
 * 走真实的 event_bus_publish(EV_FACE_EVENT) 路径：auth_fsm 与 ui_feedback 都会收到，
 * 与真机链路一致（真机是 face 线程 event_bus_post → 主线程 pump）。 */

static void post_detect(int32_t face_id, face_reason_t reason)
{
    ev_face_event_t e;
    memset(&e, 0, sizeof(e));
    e.ev = FACE_EV_DETECT;
    e.res.face_id = face_id;
    e.res.reason  = reason;
    e.res.seq     = 1;
    e.res.timestamp = (uint32_t)hal_time();
    event_bus_publish(EV_FACE_EVENT, &e);
}

static void post_enroll(safe_err_t err, int32_t face_id)
{
    ev_face_event_t e;
    memset(&e, 0, sizeof(e));
    e.ev = FACE_EV_ENROLL_DONE;
    e.enroll.face_id = face_id;
    e.enroll.err = err;
    event_bus_publish(EV_FACE_EVENT, &e);
}

static void post_delete(safe_err_t err, int32_t face_id)
{
    ev_face_event_t e;
    memset(&e, 0, sizeof(e));
    e.ev = FACE_EV_DELETE_DONE;
    e.del.face_id = face_id;
    e.del.err = err;
    event_bus_publish(EV_FACE_EVENT, &e);
}

/* 返回 true 表示确实注入了事件（调用方据此多推一段时间让动效走完淡入） */
static bool inject_face_from_env(void)
{
    const char * spec = getenv("SAFE_TEST_FACE");
    if(spec == NULL || *spec == '\0') return false;

    if(strncmp(spec, "match", 5) == 0) {
        int32_t fid = 1;                       /* 默认 admin（data/users.json: face_id=1） */
        if(spec[5] == ':') fid = (int32_t)atoi(spec + 6);
        post_detect(fid, FACE_RES_OK);
    }
    else if(strcmp(spec, "no_match") == 0)     post_detect(-1, FACE_RES_NO_MATCH);
    else if(strcmp(spec, "no_match3") == 0) {
        /* 连续三次：FSM 按 face_otp_after=3 转 WAIT_OTP，验证自动切页也带动效 */
        post_detect(-1, FACE_RES_NO_MATCH);
        post_detect(-1, FACE_RES_NO_MATCH);
        post_detect(-1, FACE_RES_NO_MATCH);
    }
    else if(strcmp(spec, "liveness") == 0)     post_detect(-1, FACE_RES_LIVENESS_FAIL);
    else if(strcmp(spec, "timeout") == 0)      post_detect(-1, FACE_RES_TIMEOUT);
    else if(strcmp(spec, "error") == 0)        post_detect(-1, FACE_RES_ERROR);
    else if(strcmp(spec, "enroll_ok") == 0)    post_enroll(SAFE_OK, 11);
    else if(strcmp(spec, "enroll_fail") == 0)  post_enroll(SAFE_ERR_FAIL, -1);
    else if(strcmp(spec, "delete_ok") == 0)    post_delete(SAFE_OK, 1);
    else if(strcmp(spec, "delete_fail") == 0)  post_delete(SAFE_ERR_FAIL, 1);
    else if(strcmp(spec, "lockout") == 0) {
        /* 走 FSM 真实失败路径：连续 max_failed 次 → LOCKOUT + EV_AUTH_RESULT */
        for(int i = 0; i < 8 && auth_fsm_state() != FSM_LOCKOUT; i++) {
            auth_fsm_note_failure("调试注入：连续失败");
        }
    }
    else {
        printf("[debug] 未知 SAFE_TEST_FACE=%s（忽略）\n", spec);
        return false;
    }
    return true;
}

/* 推 ms 毫秒的 lv_tick + timer（不能只 usleep，否则 lvgl tick 不动、动效不推进） */
static void pump_ms(int step_ms, int total_ms)
{
    for(int waited = 0; waited < total_ms; waited += step_ms) {
        usleep(step_ms * 1000);
        lv_tick_inc(step_ms);
        lv_timer_handler();
    }
}

static void take_shot(const char * path)
{
    /* PC 调试用：截屏前先注入一次「成功开锁」让 FSM = UNLOCKED，
     * 然后推 1.5s lv_tick 让 status_timer_cb (500ms) 跑至少一次，
     * 否则抓屏时 status_timer_cb 还没机会跑，状态显示滞后。
     * （这条是测试 harness 增强，不影响生产路径） */
    if(getenv("SAFE_TEST_UNLOCK")) {
        auth_fsm_note_unlock("admin");
    }
    /* 等 1500ms 让 status_timer_cb (500ms) + monitor_timer_cb (1000ms) + FPS 滑窗 (500ms) 都跑。
     * 期间每 30ms 推 lv_tick + handler 一次（不能只 usleep，否则 lvgl tick 不动 timer 不触发）。 */
    {
        const int step_ms = 30;
        const int total_ms = 3000;
        for(int waited = 0; waited < total_ms; waited += step_ms) {
            usleep(step_ms * 1000);
            lv_tick_inc(step_ms);
            lv_timer_handler();
        }
    }

    /* 人脸事件放在稳定之后注入：横幅淡入 250ms + 停留 ≤2s，
     * 注入后再推 400ms 抓屏，保证截图里横幅处于完全显示状态。 */
    if(inject_face_from_env()) {
        pump_ms(30, 400);
    }

    lv_draw_buf_t * s = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if(s == NULL) return;

    FILE * fp = fopen(path, "wb");
    if(fp != NULL) {
        uint32_t row_bytes = s->header.stride ? s->header.stride
                           : s->header.w * lv_color_format_get_bpp(s->header.cf) / 8;
        uint8_t * p8 = (uint8_t *)s->data;
        for(uint32_t y = 0; y < s->header.h; y++) {
            fwrite(p8 + y * row_bytes, 1, s->header.w * 2, fp);
        }
        fclose(fp);
    }
    lv_draw_buf_destroy(s);
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}

void debug_hooks_apply(void)
{
    const char * dlg = getenv("SAFE_TEST_DLG");
    if(dlg && *dlg && (strcmp(dlg, "add_user") == 0 ||
                       strcmp(dlg, "auth")     == 0 ||
                       strcmp(dlg, "change_pwd") == 0 ||
                       strcmp(dlg, "otp")     == 0)) {
        ui_switch_page(PAGE_USERS);
        if(strcmp(dlg, "add_user") == 0)        page_users_test_open_add_dlg();
        else if(strcmp(dlg, "auth") == 0)       page_users_test_open_auth_dlg();
        else if(strcmp(dlg, "otp") == 0)        page_users_test_open_otp_dlg();
        else                                    page_users_test_open_change_pwd_dlg();
    }

    const char * theme = getenv("SAFE_TEST_THEME");
    if(theme && *theme) theme_switch(atoi(theme));

    const char * page = getenv("SAFE_TEST_PAGE");
    if(page && *page) ui_switch_page(page_from_name(page));

    const char * shot = getenv("SAFE_TEST_SHOT");
    if(shot && *shot) take_shot(shot);
}

#else

void debug_hooks_apply(void)
{
}

#endif /* SAFE_PLATFORM_PC */
