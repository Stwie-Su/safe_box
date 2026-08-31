/**
 * @file ui.c
 * UI 外壳框架：顶部状态栏 + 底部 Tab + 内容容器。
 *
 * 布局（设计基准 1024×600，通过 ui_scale 自适应任意窗口）：
 *   顶部状态栏 48px：左 = 锁状态胶囊；中 = 实时时钟；右 = WiFi 图标。
 *   内容区：h - TOPBAR_H - TABBAR_H，各页面在此创建。
 *   底部 Tab 64px：主页 │ 日志 │ 设置。
 */
#include "ui/ui.h"
#include <time.h>            /* localtime / strftime / struct tm */
#include "hal/hal_actuator.h"    /* actuator_get_state */
#include "ui/theme.h"        /* st_screen, theme_init */
#include "ui/ui_scale.h"     /* SX/SY 自适应缩放 */
#include "ui/pages/page_monitor.h"
#include "ui/pages/page_logs.h"
#include "ui/pages/page_settings.h"
#include "ui/pages/page_users.h"
#include "ui/pages/page_network.h"
#include "ui/pages/page_system.h"
#include "ui/pages/page_keypad.h"
#include "ui/pages/page_otp.h"
#include "core/store/store.h"      /* store_init */
#include "core/support/worker.h"     /* worker_init, worker_poll */
#include "core/auth/auth_fsm.h"   /* 阶段 1：置信度状态机 + UI hook */
#include "core/remote/rpc.h"        /* 阶段 1：RPC 指令泵 + 状态上报 */

/* 基准尺寸（设计稿像素，运行时 × scale） */
#define TOPBAR_BASE  48
#define TABBAR_BASE  64

/* 页面描述符结构体：将页面 ID 映射到对应的页面创建函数*/
typedef struct {
    ui_page_t page;                            // 页面枚举 ID
    lv_obj_t * (*create)(lv_obj_t * parent);   // 函数指针，指向该页面的初始化函数
} page_desc_t;

/* 底部 Tab 导航栏的文本和图标数组（对应 0:主页, 1:日志, 2:设置） */
static const char * const TAB_LABELS[3] = { "主页", "日志", "设置" };
static const char * const TAB_ICONS[3]  = { LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_SETTINGS };

/* 注册路由表：所有需要被管理的页面都在这里注册 */
static const page_desc_t s_pages[PAGE_COUNT] = {
    { PAGE_HOME,     page_monitor_create  }, // 0: 主监控页
    { PAGE_LOGS,     page_logs_create     }, // 1: 日志列表
    { PAGE_SETTINGS, page_settings_create }, // 2: 设置主菜单
    { PAGE_USERS,    page_users_create    }, // 3: 用户管理子页
    { PAGE_NETWORK,  page_network_create  }, // 4: 网络设置子页
    { PAGE_SYSTEM,   page_system_create   }, // 5: 系统设置子页
    { PAGE_KEYPAD,   page_keypad_create   }, // 6: 密码键盘子页
    { PAGE_OTP,      page_otp_create      }, // 7: 动态密码子页
};

/* --- 全局 UI 控件句柄 --- */
static lv_obj_t * s_content;                 // 中间内容区的根容器
static lv_obj_t * s_page_roots[PAGE_COUNT];  // 存放所有初始化后的页面根节点（为了实现显隐切换）
static lv_obj_t * s_tab_btns[3];             // 存放 3 个底部 Tab 按钮的对象指针
static lv_obj_t * s_clock_label;             // 顶部时钟文本 Label
static lv_obj_t * s_lock_text;               // 顶部锁状态文本 Label
/* 主题切换时需刷新的顶栏本地颜色覆盖控件（见 topbar_refresh_theme） */
static lv_obj_t * s_capsule;                 // 顶部锁状态胶囊
static lv_obj_t * s_lock_icon;               // 胶囊内"锁"图标
static lv_obj_t * s_wifi;                    // 顶部 WiFi 图标

/* 运行时缩放后的实际高度 */
static int32_t s_topbar_h;
static int32_t s_tabbar_h;

static void build_shell(void);
static void build_topbar(lv_obj_t * parent);
static void build_tabbar(lv_obj_t * parent);
static void switch_page(ui_page_t page);
static void tab_click_cb(lv_event_t * e);
static void status_timer_cb(lv_timer_t * t);
/* 阶段 1：状态机心跳 / RPC 指令泵 / 状态周期上报 + FSM→UI 桥接 */
static void fsm_ui_hook(fsm_state_t st, const char *user, const char *detail);
/* 主题切换回调：刷新顶栏本地颜色覆盖（定义在 build_topbar 之后） */
static void topbar_refresh_theme(int idx);

/**
 * @brief GUI 启动入口，由 app.c 的 app_main 调用
 */
void ui_init(void)
{
    // 0. 初始化缩放系统（必须在所有 UI 创建之前）
    ui_scale_init();
    s_topbar_h = SY(TOPBAR_BASE);
    s_tabbar_h = SY(TABBAR_BASE);

    // 1. 基础资源初始化（存储与工作线程由 app.c 统一编排）
    app_fonts_init();
    theme_init();


    // 2.5 注册主题切换回调：刷新顶栏胶囊/锁图标/WiFi 本地颜色（须在 build_shell 之前）
    theme_register_change_cb(topbar_refresh_theme);

    // 3. 构建 UI 外壳骨架（顶栏、底栏、中间空白内容区）
    build_shell();

    // 4. 一次性实例化所有页面
    int i;
    for (i = 0; i < PAGE_COUNT; i++) {
        s_page_roots[i] = s_pages[i].create(s_content);
    }

    // 5. 创建 500ms 定时器：负责定期刷新状态栏上的时间、锁状态
    lv_timer_create(status_timer_cb, 500, NULL);
    status_timer_cb(NULL); // 立即手动调用一次

    // 6. 业务周期任务（异步结果泵、状态机心跳、指令泵、状态上报）由 app.c 统一挂载，
    //    本层只负责把状态变化翻译成界面动作。
    auth_fsm_set_ui_hook(fsm_ui_hook);

    // 7. 默认切入主页（PAGE_HOME）
    switch_page(PAGE_HOME);
}

/* FSM 状态变化 → UI 切换（业务层不直接碰 LVGL，由本 hook 桥接，NFR-5）。
 * UNLOCKED/LOCKOUT 回到主页；WAIT_OTP 弹动态码页；DENY/IDLE 不强制切页。 */
static void fsm_ui_hook(fsm_state_t st, const char *user, const char *detail)
{
    (void)user; (void)detail;
    switch (st) {
        case FSM_WAIT_OTP: ui_switch_page(PAGE_OTP);  break;
        case FSM_UNLOCKED: ui_switch_page(PAGE_HOME); break;
        case FSM_LOCKOUT:  ui_switch_page(PAGE_HOME); break;
        default: break;
    }
}

/**
 * @brief 提供给外部子页面调用的全局页面切换接口
 */
void ui_switch_page(ui_page_t page)
{
    switch_page(page);
}

static void build_shell(void)
{
    // 1. 获取当前活动屏幕对象，并配置全局背景
    lv_obj_t * scr = lv_screen_active();
    lv_obj_add_style(scr, &st_screen, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 2. 获取屏幕真实分辨率
    lv_display_t * disp = lv_display_get_default();
    int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 1024;
    int32_t h = disp ? lv_display_get_vertical_resolution(disp) : 600;

    // 3. 构建顶部状态栏容器 (TOPBAR)
    lv_obj_t * top = lv_obj_create(scr);
    lv_obj_set_size(top, w, s_topbar_h);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    // 4. 构建中部内容区容器 (CONTENT)
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, w, h - s_topbar_h - s_tabbar_h); 
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, s_topbar_h);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);

    // 5. 构建底部导航栏容器 (TABBAR)
    lv_obj_t * tab = lv_obj_create(scr);
    lv_obj_set_size(tab, w, s_tabbar_h);
    lv_obj_align(tab, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_style_pad_all(tab, 0, 0);

    // 6. 填充顶栏和底栏的具体 UI 元素
    build_topbar(top);
    build_tabbar(tab);
}

static void build_topbar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), lv_pct(100));
    lv_obj_add_style(bar, &st_panel, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    /* 左侧锁状态胶囊 */
    s_capsule = lv_obj_create(bar);
    lv_obj_set_size(s_capsule, LV_SIZE_CONTENT, SY(32));
    lv_obj_align(s_capsule, LV_ALIGN_LEFT_MID, SX(16), 0);
    lv_obj_set_style_bg_color(s_capsule, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_capsule, SX(16), 0);
    lv_obj_set_style_border_width(s_capsule, 0, 0);
    lv_obj_set_style_pad_left(s_capsule, SX(10), 0);
    lv_obj_set_style_pad_right(s_capsule, SX(12), 0);
    lv_obj_set_style_pad_top(s_capsule, 0, 0);
    lv_obj_set_style_pad_bottom(s_capsule, 0, 0);
    lv_obj_set_flex_flow(s_capsule, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_capsule, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lock_icon = lv_label_create(s_capsule);
    lv_label_set_text(s_lock_icon, "锁");
    lv_obj_set_style_text_font(s_lock_icon, app_font_scaled(16), 0);
    lv_obj_set_style_text_color(s_lock_icon, theme_color(TH_ACCENT), 0);

    s_lock_text = lv_label_create(s_capsule);
    lv_label_set_text(s_lock_text, "已上锁");
    lv_obj_add_style(s_lock_text, &st_text, 0);
    lv_obj_set_style_text_font(s_lock_text, app_font_scaled(14), 0);
    lv_obj_set_style_pad_left(s_lock_text, SX(4), 0);

    /* 中央实时时钟 */
    s_clock_label = lv_label_create(bar);
    lv_obj_add_style(s_clock_label, &st_text, 0);
    lv_obj_set_style_text_font(s_clock_label, app_font_scaled(20), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);

    /* 右侧 WiFi 图标 */
    s_wifi = lv_label_create(bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi, theme_color(TH_TEXT_MUT), 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -SX(20), 0);
}

/* ★ 主题切换回调：刷新顶栏那几个用了本地颜色覆盖的控件
 * （胶囊背景/锁图标/WiFi 图标），否则换主题时这三个不会跟着变色。 */
static void topbar_refresh_theme(int idx)
{
    (void)idx;
    if (s_capsule)  lv_obj_set_style_bg_color(s_capsule, theme_color(TH_PANEL2), 0);
    if (s_lock_icon) lv_obj_set_style_text_color(s_lock_icon, theme_color(TH_ACCENT), 0);
    if (s_wifi)     lv_obj_set_style_text_color(s_wifi, theme_color(TH_TEXT_MUT), 0);
}

static void build_tabbar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    int i;
    for (i = 0; i < 3; i++) {
        lv_obj_t * btn = lv_button_create(bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        lv_obj_add_style(btn, &st_tab_btn, 0);
        lv_obj_add_style(btn, &st_tab_btn_checked, LV_STATE_CHECKED);

        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t * icon = lv_label_create(btn);
        lv_label_set_text(icon, TAB_ICONS[i]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);  /* Montserrat has LVGL symbols */

        /* ★ 修复中文乱码：显式设置中文字体 */
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TAB_LABELS[i]);
        lv_obj_set_style_text_font(lbl, app_font_scaled(14), 0);

        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_tab_btns[i] = btn;
    }
}

static ui_page_t tab_of_page(ui_page_t page)
{
    switch (page) {
        case PAGE_LOGS:    return PAGE_LOGS;
        case PAGE_SETTINGS:
        case PAGE_USERS:
        case PAGE_NETWORK:
        case PAGE_SYSTEM:  return PAGE_SETTINGS;
        default:           return PAGE_HOME;
    }
}

static void switch_page(ui_page_t page)
{
    int i;
    if (page >= PAGE_COUNT) return;
    ui_page_t tab = tab_of_page(page);
    for (i = 0; i < 3; i++) {
        lv_obj_remove_state(s_tab_btns[i], LV_STATE_CHECKED);
    }
    lv_obj_add_state(s_tab_btns[(int)tab], LV_STATE_CHECKED);
    for (i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_hidden(s_page_roots[i], i != (int)page);
    }
}

static void tab_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    switch_page((ui_page_t)idx);
}

static void status_timer_cb(lv_timer_t * t)
{
    (void)t;

    char buf[32];
    time_t now = time(NULL);
    struct tm * tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%H:%M", tmv);
        lv_label_set_text(s_clock_label, buf);
    }

    bool open = hal_actuator_state();
    lv_label_set_text(s_lock_text, open ? "已开锁" : "已上锁");
}




