/**
 * @file ui.c
 * UI 外壳框架：顶部状态栏 + 底部 Tab + 内容容器。
 *
 * 布局（1024×600 目标屏）：
 *   顶部状态栏 48px：左 = 锁状态胶囊；中 = 实时时钟；右 = WiFi 图标。
 *   内容区：h - TOPBAR_H - TABBAR_H，各页面在此创建。
 *   底部 Tab 64px：主页 │ 日志 │ 设置。
 */
#include "ui/ui.h"
// ... (头文件省略，包含了各子页面和底层核心模块) ...

#define TOPBAR_H  48  // 顶部状态栏高度
#define TABBAR_H  64  // 底部导航栏高度

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
};

/* --- 全局 UI 控件句柄 --- */
static lv_obj_t * s_content;                 // 中间内容区的根容器
static lv_obj_t * s_page_roots[PAGE_COUNT];  // 存放所有初始化后的页面根节点（为了实现显隐切换）
static lv_obj_t * s_tab_btns[3];             // 存放 3 个底部 Tab 按钮的对象指针
static lv_obj_t * s_clock_label;             // 顶部时钟文本 Label
static lv_obj_t * s_lock_text;               // 顶部锁状态文本 Label

static void build_shell(void);
static void build_topbar(lv_obj_t * parent);
static void build_tabbar(lv_obj_t * parent);
static void switch_page(ui_page_t page);
static void tab_click_cb(lv_event_t * e);
static void status_timer_cb(lv_timer_t * t);
static void worker_poll_timer_cb(lv_timer_t * t);

/**
 * @brief GUI 启动入口，在 main 函数中被调用
 */
void app_start(void)
{
    // 1. 基础资源初始化
    app_fonts_init();
    theme_init();
    store_init();
    
    // 2. 启动后台异步工作线程（核心架构机制）
    // 使得 UI 线程即使执行复杂的哈希计算、网络请求也不会卡顿
    worker_init();          

    // 3. 构建 UI 外壳骨架（顶栏、底栏、中间空白内容区）
    build_shell();

    // 4. 一次性实例化所有页面
    // 遍历 s_pages 数组，调用所有页面的 create 函数，把它们统一挂载到存放页面根节点的数组中。
    int i;
    for (i = 0; i < PAGE_COUNT; i++) {
        s_page_roots[i] = s_pages[i].create(s_content);
    }

    // 5. 创建 500ms 定时器：负责定期刷新状态栏上的时间、锁状态
    lv_timer_create(status_timer_cb, 500, NULL);
    status_timer_cb(NULL); // 立即手动调用一次，防止刚开机时状态栏空置 500ms

    // 6. 创建 20ms 定时器：这是异步任务的“结果泵”
    // 它以极高的频率检查后台 worker 是否完成了任务，若完成则触发对应的 UI 回调
    lv_timer_create(worker_poll_timer_cb, 20, NULL);   

    // 7. 默认切入主页（PAGE_HOME）
    switch_page(PAGE_HOME);
}

/**
 * @brief 轮询后台任务的定时器回调
 */
static void worker_poll_timer_cb(lv_timer_t * t)
{
    (void)t; // 抑制未使用参数的编译警告
    worker_poll(); // 派发后台完成的作业回调到主线程
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
    lv_obj_add_style(scr, &st_screen, 0);                 // 应用自定义主题样式
    lv_obj_set_style_pad_all(scr, 0, 0);                  // 去除全屏内边距
    lv_obj_set_style_bg_color(scr, theme_color(TH_BG), 0);// 设置统一背景色
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);        // 背景完全不透明

    // 2. 获取屏幕真实分辨率，做自适应（回退值为 1024x600）
    lv_display_t * disp = lv_display_get_default();
    int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 1024;
    int32_t h = disp ? lv_display_get_vertical_resolution(disp) : 600;

    // 3. 构建顶部状态栏容器 (TOPBAR)
    lv_obj_t * top = lv_obj_create(scr);
    lv_obj_set_size(top, w, TOPBAR_H);                    // 宽度满屏，高度 48
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);            // 绝对对齐：顶部居中
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);       // 这里设为透明，样式在 build_topbar 里细化
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    // 4. 构建中部内容区容器 (CONTENT) - 所有子页面的父节点！
    s_content = lv_obj_create(scr);
    // 高度 = 总高度 - 顶栏高度 - 底栏高度
    lv_obj_set_size(s_content, w, h - TOPBAR_H - TABBAR_H); 
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, TOPBAR_H); // 紧贴着顶栏下方放置
    lv_obj_set_style_pad_all(s_content, 0, 0);              // 必须清除内边距，否则子页面会缩进
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);

    // 5. 构建底部导航栏容器 (TABBAR)
    lv_obj_t * tab = lv_obj_create(scr);
    lv_obj_set_size(tab, w, TABBAR_H);
    lv_obj_align(tab, LV_ALIGN_BOTTOM_MID, 0, 0);         // 绝对对齐：底部居中
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
    lv_obj_set_size(bar, lv_pct(100), TOPBAR_H);
    lv_obj_add_style(bar, &st_panel, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    /* 左侧锁状态胶囊：圆角 + 锁图标 + 状态文字 */
    lv_obj_t * capsule = lv_obj_create(bar);
    lv_obj_set_size(capsule, LV_SIZE_CONTENT, 32);
    lv_obj_align(capsule, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_color(capsule, theme_color(TH_PANEL2), 0);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(capsule, 16, 0);
    lv_obj_set_style_border_width(capsule, 0, 0);
    lv_obj_set_style_pad_left(capsule, 10, 0);
    lv_obj_set_style_pad_right(capsule, 12, 0);
    lv_obj_set_style_pad_top(capsule, 0, 0);
    lv_obj_set_style_pad_bottom(capsule, 0, 0);
    lv_obj_set_flex_flow(capsule, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(capsule, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * lock_icon = lv_label_create(capsule);
    lv_label_set_text(lock_icon, "锁");
    lv_obj_set_style_text_font(lock_icon, app_font(16), 0);
    lv_obj_set_style_text_color(lock_icon, theme_color(TH_ACCENT), 0);

    s_lock_text = lv_label_create(capsule);
    lv_label_set_text(s_lock_text, "已上锁");
    lv_obj_add_style(s_lock_text, &st_text, 0);
    lv_obj_set_style_text_font(s_lock_text, app_font(14), 0);
    lv_obj_set_style_pad_left(s_lock_text, 4, 0);

    /* 中央实时时钟：绝对居中 */
    s_clock_label = lv_label_create(bar);
    lv_obj_add_style(s_clock_label, &st_text, 0);
    lv_obj_set_style_text_font(s_clock_label, app_font(20), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);

    /* 右侧 WiFi 图标 */
    lv_obj_t * wifi = lv_label_create(bar);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi, theme_color(TH_TEXT_MUT), 0);
    lv_obj_align(wifi, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void build_tabbar(lv_obj_t * parent)
{
// 将整个底栏设置为水平 Flex 布局
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    int i;
    for (i = 0; i < 3; i++) {
        // 创建三个 Tab 按钮
        lv_obj_t * btn = lv_button_create(bar);
        lv_obj_set_flex_grow(btn, 1);       // 核心：让三个按钮均分宽度 (相当于 flex: 1)
        lv_obj_set_height(btn, lv_pct(100)); // 高度占满父容器 (100%)
        lv_obj_add_style(btn, &st_tab_btn, 0); // 添加普通状态样式
        lv_obj_add_style(btn, &st_tab_btn_checked, LV_STATE_CHECKED); // 添加选中状态样式

        /* 
         * 注意这里的防坑设计注释！
         * 在 LVGL v9 中，如果不直接把 Label 画在 Button 上，而是中间套一个 Obj 容器，
         * 那个 Obj 会拦截点击事件，导致 Button 永远触发不了 CLICKED。
         */
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN); // 按钮内部垂直排列（上图标、下文字）
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // 创建图标和文字
        lv_obj_t * icon = lv_label_create(btn);
        lv_label_set_text(icon, TAB_ICONS[i]);
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TAB_LABELS[i]);

        // 绑定点击事件，将当前按钮的索引 (i) 作为 user_data 传给回调函数
        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_tab_btns[i] = btn; // 保存按钮句柄，用于切换页面时改变其高亮状态
    }
}

/**
 * @brief 页面到 Tab 的归属映射器
 * 因为系统有 7 个页面，但只有 3 个底栏按钮，进入子页面时底栏对应的父 Tab 需要保持高亮。
 */
static ui_page_t tab_of_page(ui_page_t page)
{
    switch (page) {
        case PAGE_LOGS:    return PAGE_LOGS; // 映射到 Tab 1
        case PAGE_SETTINGS:
        case PAGE_USERS:
        case PAGE_NETWORK:
        case PAGE_SYSTEM:  return PAGE_SETTINGS; // 这些都属于设置的子页面，映射到 Tab 2
        default:           return PAGE_HOME; // 默认（包括主页、密码键盘）映射到 Tab 0
    }
}

/**
 * @brief 页面切换的核心控制函数（采用“控件显示/隐藏”模式的平铺式路由）
 * @param page 目标切入的页面 ID（枚举类型）
 */
static void switch_page(ui_page_t page)
{
    int i;
    // 1. 防御性安全检查：若传入的页面 ID 超出允许范围，立即退出，防止后续数组越界崩溃
    if (page >= PAGE_COUNT) return;
    // 2. 映射关联：调用辅助函数，找出当前页面属于 3 个底部 Tab 导航中的哪一个
    ui_page_t tab = tab_of_page(page);
    // 3. 重置底部导航栏：清除所有 3 个 Tab 按钮的“选中/高亮”状态（LV_STATE_CHECKED）
    for (i = 0; i < 3; i++) {
        lv_obj_remove_state(s_tab_btns[i], LV_STATE_CHECKED);
    }
    // 4. 激活当前导航：给目标页面所属的 Tab 按钮添加“选中/高亮”状态，更新视觉UI反馈
    lv_obj_add_state(s_tab_btns[(int)tab], LV_STATE_CHECKED);
    // 5. 批量控制页面显示：遍历所有注册在 s_page_roots 数组中的页面根容器
    for (i = 0; i < PAGE_COUNT; i++) {
        // 当 i == page 时，表达式 (i != page) 为 false，即不隐藏（显示该页面）
        // 当 i != page 时，表达式 (i != page) 为 true， 即隐藏该页面
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

    bool open = actuator_get_state();
    lv_label_set_text(s_lock_text, open ? "已开锁" : "已上锁");
}
