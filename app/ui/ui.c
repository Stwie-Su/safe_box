/**
 * @file ui.c
 * UI 外壳框架：左侧图标导航栏（rail）+ 顶部状态栏 + 内容容器。
 *
 * 布局（设计基准 1024×600，通过 ui_scale 自适应任意窗口）：
 *   左侧 rail 104px：品牌锁徽标 + 7 个导航项（主页/用户/日志/人脸/网络/设置/系统），
 *     每项 = 图标字体图标 + 小标签，激活项左侧 4px accent 指示条 + accent-soft 底色；
 *     右侧 1px 分隔线（TH_BORDER）。
 *   顶部状态栏 56px（位于 rail 右侧）：左 = 锁状态点 + 文本；中 = 实时时钟；
 *     右 = MQTT 连接状态点 + WiFi。
 *   内容区：剩余全部空间（w - rail_w，h - topbar_h），各页面在此创建。
 *
 * v4 变更（UI 重设计 ui4 · 里程碑 M1 后半）：
 *   - 底部 3 tab 栏 → 左侧 104px 图标 rail（视觉真源 ui_redesign_preview.html）
 *   - rail 图标全部走编译进固件的图标字体（icon_label_colored + ui_glyph_t），
 *     颜色一律取 theme.c token，四套主题自动跟随
 *   - 页面过渡由「纵向 24px 位移」改为「横向 46px 位移 + 淡入淡出」，方向随
 *     rail 上下顺序切换（向下 = 新页自右滑入，向上 = 新页自左滑入）
 */
#include "ui/ui.h"
#include "hal/hal_time.h"    /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include <time.h>            /* localtime / strftime / struct tm（只做格式化，不取时） */
#include "hal/hal_actuator.h"    /* actuator_get_state */
#include "core/auth/auth_fsm.h"    /* 顶栏解锁显示：执行器高电平 OR FSM_UNLOCKED */
#include "core/remote/mqtt_client.h" /* mqtt_is_connected：顶栏连接状态点 */
#include "ui/theme.h"        /* st_screen, theme_init */
#include "ui/ui_scale.h"     /* SX/SY 自适应缩放 */
#include "ui/ui_anim.h"      /* 动效常量集中地（UI 现代化 spec §5.2） */
#include "ui/ui_feedback.h"  /* 反馈中枢：横幅（spec §1） */
#include <stdlib.h>            /* getenv */
#include "ui/icons.h"        /* 矢量图标库 + 图标字体 label */
#include "ui/pages/page_monitor.h"
#include "ui/pages/page_logs.h"
#include "ui/pages/page_settings.h"
#include "ui/pages/page_users.h"
#include "ui/pages/page_network.h"
#include "ui/pages/page_system.h"
#include "ui/pages/page_keypad.h"
#include "ui/pages/page_otp.h"
#include "ui/pages/page_face.h"
#include "core/store/store.h"      /* store_init */
#include "core/support/worker.h"     /* worker_init, worker_poll */
#include "core/auth/auth_fsm.h"   /* 阶段 1：置信度状态机 + UI hook */
#include "core/remote/rpc.h"        /* 阶段 1：RPC 指令泵 + 状态上报 */

/* 基准尺寸（设计稿像素，运行时 × scale） */
#define TOPBAR_BASE  56
#define RAIL_BASE    104

/* 页面描述符结构体：将页面 ID 映射到对应的页面创建函数*/
typedef struct {
    ui_page_t page;                            // 页面枚举 ID
    lv_obj_t * (*create)(lv_obj_t * parent);   // 函数指针，指向该页面的初始化函数
} page_desc_t;

/* rail 导航项：图标字体码位 + 中文标签 + 目标页面（顺序 = 视觉从上到下） */
typedef struct {
    const char * label;
    ui_glyph_t   glyph;
    ui_page_t    page;
} rail_def_t;

/* 7 个入口（对齐 ui_redesign_preview.html 的 .nav 顺序） */
#define RAIL_COUNT 7
static const rail_def_t RAILS[RAIL_COUNT] = {
    { "主页", UI_GLYPH_HOME,     PAGE_HOME     },
    { "用户", UI_GLYPH_PERSON,   PAGE_USERS    },
    { "日志", UI_GLYPH_LIST,     PAGE_LOGS     },
    { "人脸", UI_GLYPH_FACE,     PAGE_FACE     },
    { "网络", UI_GLYPH_WIFI,     PAGE_NETWORK  },
    { "设置", UI_GLYPH_SETTINGS, PAGE_SETTINGS },
    { "系统", UI_GLYPH_SYSTEM,   PAGE_SYSTEM   },
};

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
    { PAGE_FACE,     page_face_create     }, // 8: 人脸识别全屏页
};

/* --- 全局 UI 控件句柄 --- */
static lv_obj_t * s_content;                 // 中间内容区的根容器
static lv_obj_t * s_overlay;                 // 横幅浮层（常驻、透明、不挡触摸；ui_feedback 的宿主）
static lv_obj_t * s_page_roots[PAGE_COUNT];  // 存放所有初始化后的页面根节点（为了实现显隐切换）
/* 当前「名义可见」的页面（过渡动画的目标页）。-1 = 尚未切过页（ui_init 首切）。
 * 快速连点时以它为准：旧目标页也按普通退场处理，不会留下半透明的孤儿页。 */
static int s_vis_page = -1;
/* 左侧 rail：7 个导航项（按钮 + 图标字体 label + 文字）+ 唯一激活指示条 */
static lv_obj_t * s_rail_btn[RAIL_COUNT];
static lv_obj_t * s_rail_icon[RAIL_COUNT];
static lv_obj_t * s_rail_label[RAIL_COUNT];
static lv_obj_t * s_rail_ind;
static int s_rail_idx = -1;                  // 当前激活的 rail 项（-1 = 无）
static lv_obj_t * s_clock_label;             // 顶部时钟文本 Label
static lv_obj_t * s_lock_text;               // 顶部锁状态文本 Label
static lv_obj_t * s_lock_dot;                // 顶部锁状态点（绿=开 / 蓝=锁）
/* 主题切换时需刷新的顶栏本地颜色覆盖控件（见 topbar_refresh_theme） */
static lv_obj_t * s_capsule;                 // 顶部锁状态胶囊
static lv_obj_t * s_mqtt_dot;                // 顶部 MQTT 连接状态点（绿=在线 / 红=离线）
static lv_obj_t * s_mqtt_text;              // 顶部 MQTT 文本（"MQTT 已连" / "MQTT 断线"）
static lv_obj_t * s_mqtt_chip;              // 顶部 MQTT chip 背景（淡灰）
static lv_obj_t * s_wifi;                    // 顶部 WiFi 图标（保留兼容）
static lv_obj_t * s_wifi_text;               // 顶部 WiFi 文本（"WiFi 5G"）

/* 运行时缩放后的实际尺寸 */
static int32_t s_topbar_h;
static int32_t s_rail_w;

static void build_shell(void);
static void build_topbar(lv_obj_t * parent);
static void build_rail(lv_obj_t * parent);
static void build_overlay(void);
static void switch_page(ui_page_t page);
/* 页面切换过渡（UI 现代化 spec §3） */
static void page_translate_x_cb(void * obj, int32_t v);
static void page_opa_cb(void * obj, int32_t v);
static void page_faded_out_cb(lv_anim_t * a);
static void page_reset(lv_obj_t * pg);
static void page_anim_out(lv_obj_t * pg, int dir);
static void page_anim_in(lv_obj_t * pg, int dir);
static void ui_page_transition_to(ui_page_t page, int dir);
static void rail_click_cb(lv_event_t * e);
static void status_timer_cb(lv_timer_t * t);
/* 阶段 1：状态机心跳 / RPC 指令泵 / 状态周期上报 + FSM→UI 桥接 */
static void fsm_ui_hook(fsm_state_t st, const char *user, const char *detail);
/* 主题切换回调：刷新顶栏本地颜色覆盖（定义在 build_topbar 之后） */
static void topbar_refresh_theme(int idx);
static void rail_refresh_theme(int idx);

/* "当前是否开锁"判定（与 page_monitor.c::is_unlocked_now 一致）：
 *   物理执行器 500ms 高电平 OR FSM UNLOCKED 30s 语义窗口 */
static inline bool is_unlocked_now(void)
{
    return hal_actuator_state() || auth_fsm_state() == FSM_UNLOCKED;
}

/**
 * @brief GUI 启动入口，由 app.c 的 app_main 调用
 */
void ui_init(void)
{
    // 0. 初始化缩放系统（必须在所有 UI 创建之前）
    ui_scale_init();
    s_topbar_h = SY(TOPBAR_BASE);
    s_rail_w = SX(RAIL_BASE);

    // 1. 基础资源初始化（存储与工作线程由 app.c 统一编排）
    app_fonts_init();
    theme_init();
    /* 图标字体自检：SAFE_ICON_CHECK=1 时逐码位打印命中情况（换字体后自证用） */
    if (getenv("SAFE_ICON_CHECK")) icon_font_selfcheck();

    // 2.5 注册主题切换回调
    theme_register_change_cb(topbar_refresh_theme);
    theme_register_change_cb(rail_refresh_theme);

    // 3. 构建 UI 外壳骨架（左 rail、顶栏、中间空白内容区）
    build_shell();

    // 4. 一次性实例化所有页面
    int i;
    for (i = 0; i < PAGE_COUNT; i++) {
        s_page_roots[i] = s_pages[i].create(s_content);
    }

    // 5. 创建 500ms 定时器：负责定期刷新状态栏上的时间、锁状态
    lv_timer_create(status_timer_cb, 500, NULL);
    status_timer_cb(NULL); // 立即手动调用一次

    // 6. 业务周期任务由 app.c 统一挂载
    auth_fsm_set_ui_hook(fsm_ui_hook);

    // 7. 默认切入主页（PAGE_HOME）
    switch_page(PAGE_HOME);

    // 8. 反馈中枢（最后初始化：依赖上面的 overlay，且要在 auth_fsm 之后订阅总线，
    //    才能保证读到的 fail_streak 已被 core 更新——见 ui_feedback.c 文件头顺序约束）
    ui_feedback_init(s_overlay);
}

/* FSM 状态变化 → UI 切换
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

lv_obj_t * ui_icon_text_button(lv_obj_t * parent,
                               const char * icon,
                               const char * text,
                               int32_t w, int32_t h,
                               lv_style_t * style,
                               lv_color_t text_color,
                               lv_event_cb_t cb,
                               void * user_data)
{
    lv_obj_t * btn = lv_button_create(parent);
    if (w > 0 && h > 0) {
        lv_obj_set_size(btn, w, h);
    } else {
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    if (style) lv_obj_add_style(btn, style, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, SX(6), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t * il = lv_label_create(btn);
    lv_label_set_text(il, icon);
    lv_obj_set_style_text_font(il, app_montserrat_scaled(14), 0);
    lv_obj_set_style_text_color(il, text_color, 0);

    lv_obj_t * tl = lv_label_create(btn);
    lv_label_set_text(tl, text);
    lv_obj_set_style_text_font(tl, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(tl, text_color, 0);
    return btn;
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

    // 3. 构建左侧图标导航栏容器（RAIL，满高）
    lv_obj_t * rail = lv_obj_create(scr);
    lv_obj_set_size(rail, s_rail_w, h);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(rail, 0, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_outline_width(rail, 0, 0);

    // 4. 构建顶部状态栏容器（TOPBAR，位于 rail 右侧）
    lv_obj_t * top = lv_obj_create(scr);
    lv_obj_set_size(top, w - s_rail_w, s_topbar_h);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_outline_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    // 5. 构建中部内容区容器（CONTENT，rail 右侧 + 顶栏下方）
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, w - s_rail_w, h - s_topbar_h);
    lv_obj_align(s_content, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_outline_width(s_content, 0, 0);

    // 6. 填充 rail 与顶栏的具体 UI 元素
    build_rail(rail);
    build_topbar(top);

    // 7. 横幅浮层（最上层）：常驻、透明、不挡触摸，供 ui_feedback 挂反馈条
    build_overlay();
}

/**
 * @brief 横幅浮层（spec §1「overlay 横幅容器（常驻、默认空、不挡触摸）」）。
 *
 * 实现取舍：不占纵向布局空间——用绝对定位浮在内容区顶部，而不是插进纵向
 * flex 里（插进去会让内容区在横幅出现/消失时整体抖动，且空态也要吃掉 ~64px）。
 * 容器与横幅都去掉 CLICKABLE，触摸事件穿透到下面的页面。
 */
static void build_overlay(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_display_t * disp = lv_display_get_default();
    int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 1024;

    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, w - s_rail_w, SY(UI_ANIM_BANNER_H_BASE) + SY(12));
    lv_obj_align(s_overlay, LV_ALIGN_TOP_RIGHT, 0, s_topbar_h + SY(6));
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_outline_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(s_overlay, false);
    lv_obj_set_scrollable(s_overlay, false);
}

static void build_topbar(lv_obj_t * parent)
{
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), lv_pct(100));
    lv_obj_add_style(bar, &st_panel, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    /* 左侧锁状态胶囊：状态点 + 文本 */
    s_capsule = lv_obj_create(bar);
    lv_obj_set_size(s_capsule, LV_SIZE_CONTENT, SY(32));
    lv_obj_align(s_capsule, LV_ALIGN_LEFT_MID, SX(16), 0);
    lv_obj_set_style_bg_color(s_capsule, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_capsule, SX(16), 0);
    lv_obj_set_style_border_width(s_capsule, 0, 0);
    lv_obj_set_style_pad_left(s_capsule, SX(12), 0);
    lv_obj_set_style_pad_right(s_capsule, SX(14), 0);
    lv_obj_set_style_pad_top(s_capsule, 0, 0);
    lv_obj_set_style_pad_bottom(s_capsule, 0, 0);
    lv_obj_set_flex_flow(s_capsule, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_capsule, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lock_dot = lv_obj_create(s_capsule);
    lv_obj_set_size(s_lock_dot, SX(10), SX(10));
    lv_obj_set_style_radius(s_lock_dot, LV_RADIUS_CIRCLE, 0);
    /* 默认按「已上锁」渲染：红色（解锁后会由 status_timer_cb 改为绿色） */
    lv_obj_set_style_bg_color(s_lock_dot, theme_color(TH_DANGER), 0);
    lv_obj_set_style_bg_opa(s_lock_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lock_dot, 0, 0);

    s_lock_text = lv_label_create(s_capsule);
    lv_label_set_text(s_lock_text, "已上锁");
    lv_obj_add_style(s_lock_text, &st_text, 0);
    lv_obj_set_style_text_font(s_lock_text, app_font_scaled(14), 0);
    lv_obj_set_style_pad_left(s_lock_text, SX(6), 0);

    /* 中央实时时钟 */
    s_clock_label = lv_label_create(bar);
    lv_obj_add_style(s_clock_label, &st_text, 0);
    lv_obj_set_style_text_font(s_clock_label, app_font_scaled(20), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);

    /* 右侧：WiFi chip + MQTT chip
     *   [📶] WiFi 5G   [🟢] MQTT 已连
     * 每个 chip = 小淡灰圆角背景 + 矢量图标 + 文字标签。 */
    lv_obj_t * right = lv_obj_create(bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, SY(36));
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -SX(16), 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_outline_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_column(right, SX(8), 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ---- WiFi chip：背景 + WiFi 图标 + "WiFi 5G" 文字 ---- */
    lv_obj_t * wifi_chip = lv_obj_create(right);
    lv_obj_set_size(wifi_chip, LV_SIZE_CONTENT, SY(28));
    lv_obj_set_style_bg_color(wifi_chip, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(wifi_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_chip, 0, 0);
    lv_obj_set_style_outline_width(wifi_chip, 0, 0);
    lv_obj_set_style_radius(wifi_chip, SX(14), 0);
    lv_obj_set_style_pad_left(wifi_chip, SX(10), 0);
    lv_obj_set_style_pad_right(wifi_chip, SX(12), 0);
    lv_obj_set_style_pad_top(wifi_chip, 0, 0);
    lv_obj_set_style_pad_bottom(wifi_chip, 0, 0);
    lv_obj_set_style_pad_column(wifi_chip, SX(4), 0);
    lv_obj_set_flex_flow(wifi_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_wifi = lv_label_create(wifi_chip);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi, theme_color(TH_OK), 0);

    s_wifi_text = lv_label_create(wifi_chip);
    lv_label_set_text(s_wifi_text, "WiFi 5G");
    lv_obj_set_style_text_font(s_wifi_text, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(s_wifi_text, theme_color(TH_TEXT), 0);

    /* ---- MQTT chip：背景 + 圆点 + "MQTT 已连 / 断线" 文字 ---- */
    s_mqtt_chip = lv_obj_create(right);
    lv_obj_set_size(s_mqtt_chip, LV_SIZE_CONTENT, SY(28));
    lv_obj_set_style_bg_color(s_mqtt_chip, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(s_mqtt_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mqtt_chip, 0, 0);
    lv_obj_set_style_outline_width(s_mqtt_chip, 0, 0);
    lv_obj_set_style_radius(s_mqtt_chip, SX(14), 0);
    lv_obj_set_style_pad_left(s_mqtt_chip, SX(10), 0);
    lv_obj_set_style_pad_right(s_mqtt_chip, SX(12), 0);
    lv_obj_set_style_pad_top(s_mqtt_chip, 0, 0);
    lv_obj_set_style_pad_bottom(s_mqtt_chip, 0, 0);
    lv_obj_set_style_pad_column(s_mqtt_chip, SX(6), 0);
    lv_obj_set_flex_flow(s_mqtt_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_mqtt_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_mqtt_dot = lv_obj_create(s_mqtt_chip);
    lv_obj_set_size(s_mqtt_dot, SX(8), SX(8));
    lv_obj_set_style_radius(s_mqtt_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mqtt_dot, theme_color(TH_OK), 0);
    lv_obj_set_style_bg_opa(s_mqtt_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mqtt_dot, 0, 0);
    lv_obj_set_style_outline_width(s_mqtt_dot, 0, 0);

    s_mqtt_text = lv_label_create(s_mqtt_chip);
    lv_label_set_text(s_mqtt_text, "MQTT 已连");
    lv_obj_set_style_text_font(s_mqtt_text, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(s_mqtt_text, theme_color(TH_TEXT), 0);
}

/* 主题切换回调：刷新顶栏那几个用了本地颜色覆盖的控件 */
static void topbar_refresh_theme(int idx)
{
    (void)idx;
    bool open = is_unlocked_now();
    bool mqtt_on = mqtt_is_connected();
    if (s_capsule)     lv_obj_set_style_bg_color(s_capsule, theme_color(TH_PANEL2), 0);
    if (s_lock_dot)    lv_obj_set_style_bg_color(s_lock_dot, open ? theme_color(TH_OK) : theme_color(TH_DANGER), 0);
    if (s_mqtt_dot)    lv_obj_set_style_bg_color(s_mqtt_dot, mqtt_on ? theme_color(TH_OK) : theme_color(TH_DANGER), 0);
    if (s_mqtt_chip)   lv_obj_set_style_bg_color(s_mqtt_chip, theme_color(TH_PANEL2), 0);
    /* WiFi / MQTT 文字色（PC 阶段假定已连 = TH_OK 绿；wifi_text 走主题主色） */
    if (s_wifi)        lv_obj_set_style_text_color(s_wifi, theme_color(TH_OK), 0);
    if (s_wifi_text)   lv_obj_set_style_text_color(s_wifi_text, theme_color(TH_TEXT), 0);
    if (s_mqtt_text)   lv_obj_set_style_text_color(s_mqtt_text, mqtt_on ? theme_color(TH_TEXT) : theme_color(TH_DANGER), 0);
}

/* ================================================================
 *  左侧图标导航栏（ui4 · 对齐 ui_redesign_preview.html 的 .rail）
 *
 *  结构（设计稿 px）：
 *    rail 宽 104，右边框 1px（TH_BORDER）；
 *    品牌徽标 46×46 圆角 14，accent 浅底 + accent 锁图标，上边距 18；
 *    7 个导航项 80×64 圆角 16，纵向间距 6，图标 24px + 标签 13px；
 *    激活项：accent 文字 + accent-soft 底色 + 左侧 4px accent 指示条。
 *
 *  为什么不用 flex：rail 是「固定数量 + 固定尺寸」的纵向栈，手工 set_pos
 *  更简单，也让唯一的指示条能自由绝对定位（不受 flex 布局约束）。
 *  颜色一律 theme_color(token)，底色用 accent + 低 opa 模拟 accent-soft，
 *  无任何硬编码 hex，四套主题自动跟随。
 * ================================================================ */

/* 第 i 个导航项的 y（设计稿 px → 运行时缩放） */
static inline int32_t rail_item_y(int i)
{
    return SY(18) + SY(46) + SY(14) + i * (SY(64) + SY(6));
}

static void build_rail(lv_obj_t * parent)
{
    /* 外层连续面板（st_panel 样式 + 右侧 1px 分隔线） */
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), lv_pct(100));
    lv_obj_add_style(bar, &st_panel, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(bar, theme_color(TH_BORDER), 0);
    lv_obj_set_style_outline_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollable(bar, false);

    /* 品牌徽标：accent 浅底 + accent 锁图标 */
    lv_obj_t * brand = lv_obj_create(bar);
    lv_obj_set_size(brand, SX(46), SY(46));
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, SY(18));
    lv_obj_set_style_bg_color(brand, theme_color(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(brand, LV_OPA_30, 0);
    lv_obj_set_style_radius(brand, SX(14), 0);
    lv_obj_set_style_border_width(brand, 0, 0);
    lv_obj_set_style_outline_width(brand, 0, 0);
    lv_obj_set_style_pad_all(brand, 0, 0);
    lv_obj_set_scrollable(brand, false);
    lv_obj_t * brand_ic = icon_label_colored(brand, UI_GLYPH_LOCK, 26, TH_ACCENT);
    lv_obj_center(brand_ic);

    for (int i = 0; i < RAIL_COUNT; i++) {
        /* 导航项容器：手工定位（见上方「为什么不用 flex」） */
        lv_obj_t * btn = lv_obj_create(bar);
        lv_obj_set_size(btn, SX(80), SY(64));
        lv_obj_set_pos(btn, SX(12), rail_item_y(i));
        lv_obj_set_style_radius(btn, SX(16), 0);
        lv_obj_set_style_bg_color(btn, theme_color(TH_ACCENT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);                 /* 常态透明 */
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_CHECKED);      /* 激活：accent-soft */
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);      /* 按压：略强 */
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUSED);   /* 触摸聚焦不留外框 */
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_clickable(btn, true);
        lv_obj_set_scrollable(btn, false);

        /* 图标字体图标（24px → lv_font_icon_24） */
        lv_obj_t * icon = icon_label_colored(btn, RAILS[i].glyph, 24, TH_TEXT_MUT);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, SY(10));
        lv_obj_add_style(icon, &st_tab_hl, 0);      /* 高亮切换 150ms 渐变 */
        s_rail_icon[i] = icon;

        /* 文字标签 */
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, RAILS[i].label);
        lv_obj_set_style_text_font(lbl, app_font_scaled(13), 0);
        lv_obj_set_style_text_color(lbl, theme_color(TH_TEXT_MUT), 0);
        lv_obj_add_style(lbl, &st_tab_hl, 0);       /* 高亮切换 150ms 渐变 */
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -SY(8));
        s_rail_label[i] = lbl;

        lv_obj_add_event_cb(btn, rail_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_rail_btn[i] = btn;
    }

    /* 激活指示条：全 rail 唯一对象，随选中项移动到对应导航项左侧 */
    s_rail_ind = lv_obj_create(bar);
    lv_obj_set_size(s_rail_ind, SX(4), SY(36));
    lv_obj_set_style_bg_color(s_rail_ind, theme_color(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_rail_ind, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rail_ind, SX(2), 0);
    lv_obj_set_style_border_width(s_rail_ind, 0, 0);
    lv_obj_set_style_outline_width(s_rail_ind, 0, 0);
    lv_obj_set_scrollable(s_rail_ind, false);

    s_rail_idx = -1;
    rail_refresh_theme(-1);   /* 触发一次 rail 主题色初始化 */
}

/* 主题切换回调：刷新 rail 选中/非选中配色 + 指示条位置。
 * idx=-1 表示首次进入（无选中项，指示条隐藏）。 */
static void rail_refresh_theme(int idx)
{
    (void)idx;
    for (int i = 0; i < RAIL_COUNT; i++) {
        bool selected = (i == s_rail_idx);
        lv_color_t col = selected ? theme_color(TH_ACCENT) : theme_color(TH_TEXT_MUT);
        if (s_rail_btn[i])   lv_obj_set_style_bg_color(s_rail_btn[i], theme_color(TH_ACCENT), 0);
        if (s_rail_icon[i])  lv_obj_set_style_text_color(s_rail_icon[i], col, 0);
        if (s_rail_label[i]) lv_obj_set_style_text_color(s_rail_label[i], col, 0);
    }
    if (s_rail_ind) {
        lv_obj_set_style_bg_color(s_rail_ind, theme_color(TH_ACCENT), 0);
        if (s_rail_idx >= 0) {
            lv_obj_set_pos(s_rail_ind, SX(2), rail_item_y(s_rail_idx) + SY(14));
            lv_obj_set_hidden(s_rail_ind, false);
        } else {
            lv_obj_set_hidden(s_rail_ind, true);
        }
    }
}

/* 返回页面所属的 rail 项索引；不在 rail 上的页面（键盘/动态码全屏层）返回 -1
 * ——此时保持当前高亮不变，避免全屏层弹出时 rail 闪动。 */
static int rail_index_of(ui_page_t page)
{
    for (int i = 0; i < RAIL_COUNT; i++) {
        if (RAILS[i].page == page) return i;
    }
    return -1;
}

/* ================================================================
 *  页面切换过渡（UI 现代化 spec §3）
 *
 *  新页：translate_x ±46px → 0 + opa 0 → 255，200ms ease-out（入场减速）
 *  旧页：translate_x 0 → ∓46px + opa 255 → 0，160ms ease-out（退场略快，
 *        避免两段同时占用重绘带宽）
 *  方向：dir = +1（沿 rail 向下选）→ 新页自右滑入、旧页向左退出；
 *        dir = -1（沿 rail 向上选）→ 新页自左滑入、旧页向右退出。
 *
 *  为什么自建 lv_anim 而不是 lv_screen_load_anim：
 *    lv_screen_load_anim 的粒度是整个 screen，会把常驻的顶栏 / rail 一起
 *    换掉（shell 与页面是同一 screen 的父子关系）。spec §3 也给出「或自建
 *    lv_anim 对两个 screen 做 x/opacity 插值」这一路径，故在页面根节点上
 *    做等价的 x + opacity 插值，视觉与 MOVE_LEFT / MOVE_RIGHT 一致。
 *
 *  结构与性能要点（与旧版一致，未回退）：
 *   - 页面 roots 常驻（ui_init 一次性实例化），动画只动 translate_x / opa，
 *     不销毁、不重建对象（A7 单核：局部重绘 content 区）；
 *   - 快速连点：每个对象同一属性只留一条动画——起动画前先 lv_anim_delete 清旧，
 *     并把「上一个目标页」也按退场处理，杜绝半透明孤儿页；
 *   - 回调内不 create / free、不 malloc；
 *   - FSM 自动切页（fsm_ui_hook）走同一路径，自动获得动画；
 *   - 常量集中 ui_anim.h，A7 上调参只改那里。
 * ================================================================ */

/* 动画执行回调：只改一个样式属性，便于 lv_anim_delete 按 cb 精确清理 */
static void page_translate_x_cb(void * obj, int32_t v)
{
    if (obj == NULL) return;
    lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0);
}

static void page_opa_cb(void * obj, int32_t v)
{
    if (obj == NULL) return;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* 旧页淡出结束：隐藏 + 复位样式（下次入场从干净状态开始） */
static void page_faded_out_cb(lv_anim_t * a)
{
    (void)a;
    /* lv_anim 的 var 就是页面根节点 */
    lv_obj_t * pg = (lv_obj_t *)a->var;
    if (pg == NULL) return;
    lv_obj_set_style_translate_x(pg, 0, 0);
    lv_obj_set_style_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_hidden(pg, true);
}

/* 删动画 + 复位样式 + 隐藏（用于与本次切换无关的页面，保证状态确定） */
static void page_reset(lv_obj_t * pg)
{
    if (pg == NULL) return;
    lv_anim_delete(pg, page_translate_x_cb);
    lv_anim_delete(pg, page_opa_cb);
    lv_obj_set_style_translate_x(pg, 0, 0);
    lv_obj_set_style_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_hidden(pg, true);
}

/* 旧页退场：从当前样式值出发（可能是上次动画进行到一半的值），平滑滑出 */
static void page_anim_out(lv_obj_t * pg, int dir)
{
    if (pg == NULL) return;
    lv_anim_delete(pg, page_translate_x_cb);
    lv_anim_delete(pg, page_opa_cb);

    int32_t x0 = lv_obj_get_style_translate_x(pg, 0);
    int32_t o0 = (int32_t)lv_obj_get_style_opa(pg, 0);
    lv_obj_set_hidden(pg, false);   /* 可能在上一次过渡里已被隐藏，先亮出来退场 */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, pg);
    lv_anim_set_exec_cb(&a, page_translate_x_cb);
    lv_anim_set_values(&a, x0, -dir * UI_TRANS_SHIFT_PX);
    lv_anim_set_duration(&a, UI_TRANS_OUT_MS);
    lv_anim_set_path_cb(&a, UI_TRANS_EASE);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, pg);
    lv_anim_set_exec_cb(&a, page_opa_cb);
    lv_anim_set_values(&a, o0, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, UI_TRANS_OUT_MS);
    lv_anim_set_path_cb(&a, UI_TRANS_EASE);
    lv_anim_set_completed_cb(&a, page_faded_out_cb);
    lv_anim_start(&a);
}

/* 新页入场：从 dir 方向 46px 处滑入 + 淡入 */
static void page_anim_in(lv_obj_t * pg, int dir)
{
    if (pg == NULL) return;
    lv_anim_delete(pg, page_translate_x_cb);
    lv_anim_delete(pg, page_opa_cb);

    lv_obj_set_style_translate_x(pg, dir * UI_TRANS_SHIFT_PX, 0);
    lv_obj_set_style_opa(pg, LV_OPA_TRANSP, 0);
    lv_obj_set_hidden(pg, false);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, pg);
    lv_anim_set_exec_cb(&a, page_translate_x_cb);
    lv_anim_set_values(&a, dir * UI_TRANS_SHIFT_PX, 0);
    lv_anim_set_duration(&a, UI_TRANS_MS);
    lv_anim_set_path_cb(&a, UI_TRANS_EASE);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, pg);
    lv_anim_set_exec_cb(&a, page_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, UI_TRANS_MS);
    lv_anim_set_path_cb(&a, UI_TRANS_EASE);
    lv_anim_start(&a);
}

/**
 * @brief 带过渡的页面切换。同页重复点击不重放动画（避免无意义的闪动）。
 * @param dir  +1 新页自右滑入 / -1 新页自左滑入
 */
static void ui_page_transition_to(ui_page_t page, int dir)
{
    if (page < 0 || page >= PAGE_COUNT) return;
    int prev = s_vis_page;
    s_vis_page = (int)page;
    if (prev == (int)page) return;   /* 同页重复点击 */

    /* 与本次切换无关的页面：立即清场（删动画/复位/隐藏），状态确定 */
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)page || i == prev) continue;
        page_reset(s_page_roots[i]);
    }
    if (prev >= 0 && prev != (int)page) page_anim_out(s_page_roots[prev], dir);
    page_anim_in(s_page_roots[page], dir);
}

static void switch_page(ui_page_t page)
{
    if (page >= PAGE_COUNT) return;

    int ri = rail_index_of(page);
    int dir = 1;
    if (ri >= 0) {
        /* 沿 rail 向下选 → 新页自右滑入；向上选 → 自左滑入 */
        dir = (s_rail_idx < 0 || ri >= s_rail_idx) ? 1 : -1;
        for (int i = 0; i < RAIL_COUNT; i++) {
            if (s_rail_btn[i] == NULL) continue;
            if (i == ri) lv_obj_add_state(s_rail_btn[i], LV_STATE_CHECKED);
            else         lv_obj_remove_state(s_rail_btn[i], LV_STATE_CHECKED);
        }
        s_rail_idx = ri;
    }

    ui_page_transition_to(page, dir);
    rail_refresh_theme(0);
}

static void rail_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    switch_page(RAILS[idx].page);
}

static void status_timer_cb(lv_timer_t * t)
{
    (void)t;

    char buf[32];
    time_t now = (time_t)hal_time();
    struct tm * tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%H:%M", tmv);
        lv_label_set_text(s_clock_label, buf);
    }

    bool open = is_unlocked_now();
    lv_label_set_text(s_lock_text, open ? "已开锁" : "已上锁");
    if (s_lock_dot) lv_obj_set_style_bg_color(s_lock_dot, open ? theme_color(TH_OK) : theme_color(TH_DANGER), 0);
    if (s_mqtt_dot) lv_obj_set_style_bg_color(s_mqtt_dot, mqtt_is_connected() ? theme_color(TH_OK) : theme_color(TH_DANGER), 0);
    /* WiFi / MQTT 文字标签 */
    if (s_wifi_text)  lv_label_set_text(s_wifi_text, "WiFi 5G");
    if (s_mqtt_text)  lv_label_set_text(s_mqtt_text, mqtt_is_connected() ? "MQTT 已连" : "MQTT 断线");
}

