/**
 * @file debug_hooks.c
 * 调试钩子实现。仅 PC（SDL）构建有实质内容，其它平台为空函数。
 *
 * 环境变量：
 *   SAFE_TEST_PAGE  = HOME | LOGS | SETTINGS | USERS | NETWORK | SYSTEM | KEYPAD
 *   SAFE_TEST_THEME = 0..3
 *   SAFE_TEST_DLG   = add_user | auth | change_pwd
 *   SAFE_TEST_SHOT  = 截图输出路径（原始 RGB565，截图后直接退出）
 */

#include "platform/debug_hooks.h"

#if defined(SAFE_PLATFORM_PC)

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

static void take_shot(const char * path)
{
    /* PC 调试用：截屏前先注入一次「成功开锁」让 FSM = UNLOCKED，
     * 然后推 1.5s lv_tick 让 status_timer_cb (500ms) 跑至少一次，
     * 否则抓屏时 status_timer_cb 还没机会跑，状态显示滞后。
     * （这条是测试 harness 增强，不影响生产路径） */
    if(getenv("SAFE_TEST_UNLOCK")) {
        auth_fsm_note_unlock("admin");
    }
    /* 真实等 1200ms 让 status_timer_cb (500ms) + monitor_timer_cb (1000ms) 都跑一次 */
    usleep(1200 * 1000);
    /* 再让 lv_tick_inc + 多次 handler 处理其它（数字、动画） */
    lv_tick_inc(100);
    for(int i = 0; i < 5; i++) lv_timer_handler();

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
