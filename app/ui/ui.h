/**
 * @file ui.h
 * 界面层的入口：建好整个界面、切换页面。
 *
 * 页面结构（底部四个页签）：
 *   主页、用户、日志、设置；另外还有几个全屏页（密码键盘、动态码、人脸识别、人脸录入）。
 *
 * 分工：ui/ 只管「画出来 + 绑定点击事件」，判断逻辑一律调 core/，
 * 硬件动作一律调 hal/。界面层不写业务规则。
 *
 * 调用时机：
 *   - ui_init()：开机时 app_main() 调一次，建好所有页面；
 *   - ui_switch_page()：用户点页签或按钮时调；
 *   - 页面自己订阅总线的，在收到事件后更新（例如人脸结果）。
 */
#ifndef SAFE_UI_H
#define SAFE_UI_H

#include "lvgl.h"
#include "fonts/fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板子用的中文字体（PC 上用 FreeType 动态渲染，见 fonts.h） */
LV_FONT_DECLARE(lv_font_cn_14)
LV_FONT_DECLARE(lv_font_cn_16)
LV_FONT_DECLARE(lv_font_cn_20)
LV_FONT_DECLARE(lv_font_cn_28)

/* 页面编号。前三个是底部页签，其余是设置子页或全屏页。 */
typedef enum {
    PAGE_HOME     = 0,   /* 主页：锁状态 + 时钟 + 开锁入口 */
    PAGE_LOGS,           /* 日志 */
    PAGE_SETTINGS,       /* 开发者选项（只读诊断，不需要密码） */
    PAGE_USERS,          /* 用户管理 */
    PAGE_NETWORK,        /* 网络 */
    PAGE_SYSTEM,         /* 系统：策略 / 主题 / 恢复出厂 */
    PAGE_KEYPAD,         /* 输密码键盘（全屏） */
    PAGE_OTP,            /* 输动态码（全屏） */
    PAGE_FACE,           /* 人脸识别（全屏） */
    PAGE_ENROLL,         /* 人脸录入（全屏） */
    PAGE_COUNT
} ui_page_t;

/* 建好整个界面。开机时调一次（在 auth_fsm_init 之后）。 */
void ui_init(void);

/* 切到某个页面（隐藏其它的，不重新创建，所以很快）。 */
void ui_switch_page(ui_page_t page);

/* 建一个「左边图标 + 右边文字」的按钮，返回按钮对象。 */
lv_obj_t * ui_icon_text_button(lv_obj_t * parent,
                               const char * icon,
                               const char * text,
                               int32_t w, int32_t h,
                               lv_style_t * style,
                               lv_color_t text_color,
                               lv_event_cb_t cb,
                               void * user_data);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SAFE_UI_H*/
