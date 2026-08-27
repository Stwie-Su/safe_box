/**
 * @file page_monitor.c
 * 主页（目标 UI）：中央锁状态圆环 + 状态文字 + 最后一次开启时间 + 三个等宽信息卡。
 *
 * 从上到下：
 *   1) 大圆环（200px，描边环）：中央锁图标，锁定时强调色、开启时成功色；
 *   2) 状态主标题：保险柜已上锁 / 保险柜已开启；
 *   3) 最后一次开启：MM-DD HH:MM（来自日志最近一条 unlock 成功记录）；
 *   4) 三张等宽信息卡：用户 / 今日事件 / 网络。
 */
#include "page_monitor.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "core/store.h"
#include "core/worker.h"
#include "hal/actuator.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================== 全局静态 UI 对象指针 ================== */
/* 
 * 为什么定义为 static 全局变量？
 * 因为定时器回调函数（monitor_timer_cb）每秒都要刷新这些控件的文字或颜色，
 * 把它们存为文件级全局指针，方便在回调函数中直接调用 lv_label_set_text 等 API 进行修改。
 */
static lv_obj_t * s_lock_ring;      // 中央的大圆环
static lv_obj_t * s_lock_icon;      // 圆环中间的字（“锁”/“开”）
static lv_obj_t * s_status_label;   // 状态主标题（“保险柜已上锁”等）
static lv_obj_t * s_last_label;     // 最后一次开启时间文本
static lv_obj_t * s_user_count;     // 底部卡片1：用户数量的数值文本
static lv_obj_t * s_event_count;    // 底部卡片2：今日事件数量的数值文本
static lv_obj_t * s_net_state;      // 底部卡片3：网络状态的文本

/* ================== 内部私有函数声明 ================== */
static void monitor_timer_cb(lv_timer_t * t);
static void build_info_card(lv_obj_t * parent, const char * title, lv_obj_t ** value_lbl);
static int count_today_events(void);

/**
 * @brief 创建监控主页面（由 UI 框架调用）
 * @param parent 父容器（通常是主屏幕屏幕对象）
 * @return 创建好的当前页面的根节点 root
 */
lv_obj_t * page_monitor_create(lv_obj_t * parent)
{
    /* 1. 创建页面的根容器 root */
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100)); // 宽高占满父容器 (100%)
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0); // 背景全透明
    lv_obj_set_style_border_width(root, 0, 0);       // 取消边框
    
    /* 设置内边距 (Padding) */
    lv_obj_set_style_pad_all(root, 16, 0);           // 四周基础内边距 16px
    lv_obj_set_style_pad_row(root, 20, 0);           // Flex布局中，每行（每个子元素）之间的垂直间距 20px

    /* 【核心：弹性盒子布局 Flexbox】 
     * COLUMN：子元素从上到下垂直排列。
     * CENTER：子元素在水平方向和垂直方向都居中对齐。
     */
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN); 
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 2. 创建子元素 1)：中央锁状态大圆环 */
    s_lock_ring = lv_obj_create(root);
    lv_obj_set_size(s_lock_ring, 200, 200);                    // 强制固定尺寸 200x200
    lv_obj_set_style_radius(s_lock_ring, LV_RADIUS_CIRCLE, 0); // 圆角半径设为最大，变成纯圆形
    lv_obj_set_style_bg_opa(s_lock_ring, LV_OPA_TRANSP, 0);    // 环内背景透明
    lv_obj_set_style_border_width(s_lock_ring, 6, 0);          // 圆环粗细 6px
    lv_obj_set_style_border_color(s_lock_ring, theme_color(TH_ACCENT), 0); // 默认边框颜色为“强调色”
    
    // 让圆环自己也变成一个 Flex 容器，为了把里面的文字彻底居中
    lv_obj_set_flex_flow(s_lock_ring, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lock_ring, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 圆环中间的文字图标 */
    s_lock_icon = lv_label_create(s_lock_ring);
    lv_label_set_text(s_lock_icon, "锁");
    lv_obj_add_style(s_lock_icon, &st_text, 0);
    lv_obj_set_style_text_font(s_lock_icon, app_font(28), 0);
    lv_obj_set_style_text_color(s_lock_icon, theme_color(TH_ACCENT), 0);

    /* 3. 创建子元素 2)：状态主标题文字 */
    s_status_label = lv_label_create(root);
    lv_label_set_text(s_status_label, "保险柜已上锁");
    lv_obj_add_style(s_status_label, &st_text, 0);
    lv_obj_set_style_text_font(s_status_label, app_font(28), 0);

    /* 4. 创建子元素 3)：最后一次开启时间文字 */
    s_last_label = lv_label_create(root);
    lv_label_set_text(s_last_label, "最后一次开启：--");
    lv_obj_add_style(s_last_label, &st_text_mut, 0); // 使用 muted（次要/灰暗）文本样式
    lv_obj_set_style_text_font(s_last_label, app_font(16), 0);

    /* 5. 创建子元素 4)：底部的水平卡片容器 */
    lv_obj_t * cards = lv_obj_create(root);
    lv_obj_set_size(cards, lv_pct(100), LV_SIZE_CONTENT); // 宽度100%，高度根据内容自适应（包裹内容）
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_set_style_pad_column(cards, 16, 0); // Flex布局中，水平排列的子元素之间的间距设为 16px
    lv_obj_set_style_pad_top(cards, 12, 0);    // 距离上方元素稍微拉开12px
    
    // 这个容器是 ROW 横向排列的 Flex 盒子
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 调用封装好的函数，连续创建3个相同的卡片结构。
     * 注意这里传递了 &s_user_count 等指针的地址（二级指针），
     * 这样子函数创建好动态文本控件后，能把地址赋值给这个全局变量。 */
    build_info_card(cards, "用户", &s_user_count);
    build_info_card(cards, "今日事件", &s_event_count);
    build_info_card(cards, "网络", &s_net_state);

    /* 6. 创建一个定时器：每隔 1000 毫秒（1秒）触发一次 monitor_timer_cb */
    lv_timer_create(monitor_timer_cb, 1000, root);
    
    /* 手动触发一次定时器回调，让页面刚创建时立马就有数据显示，而不是干等1秒 */
    monitor_timer_cb(NULL);
    
    return root;
}

/**
 * @brief 辅助函数：构造一个数据信息卡片
 * @param parent 父容器（cards）
 * @param title 卡片顶部的标题（如 "用户"）
 * @param value_lbl [输出参数] 用来接收生成的数值Label对象指针，方便日后修改它的值
 */
static void build_info_card(lv_obj_t * parent, const char * title, lv_obj_t ** value_lbl)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, 160, 96);
    
    /* 【排版技巧】flex_grow(1) 表示如果父容器有多余宽度，这几个卡片会按 1:1:1 的比例均分剩余空间，实现绝对等宽 */
    lv_obj_set_flex_grow(card, 1); 
    lv_obj_add_style(card, &st_panel, 0); // 应用全局统一的“面板”样式（可能包含圆角、阴影、深色背景等）
    lv_obj_set_style_pad_all(card, 14, 0);
    
    // 卡片内部也是一个上下垂直排列的弹性盒子
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 顶部小字标题
    lv_obj_t * t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_add_style(t, &st_text_mut, 0);
    lv_obj_set_style_text_font(t, app_font(14), 0);

    // 底部大字数值
    *value_lbl = lv_label_create(card); // 通过二级指针把生成的 Label 实体传递出去
    lv_label_set_text(*value_lbl, "--");
    lv_obj_add_style(*value_lbl, &st_text, 0);
    lv_obj_set_style_text_font(*value_lbl, app_font(28), 0);
    lv_obj_set_style_pad_top(*value_lbl, 6, 0);
}

/**
 * @brief 业务函数：统计今天的日志事件数量 (涉及本地文件系统查询，比较耗时)
 */
static int count_today_events(void)
{
    log_entry_t * entries = NULL;
    int n = 0;
    // 获取全部日志。如果失败直接返回 0
    if (log_query(NULL, -1, &entries, &n) != 0) return 0;

    // 获取当前的系统时间，并格式化为 "YYYY-MM-DD" 的字符串（如 "2026-08-27"）
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);

    int cnt = 0;
    // 遍历所有日志
    for (int i = 0; i < n; i++) {
        // 取日志时间戳的前10个字符跟今天的日期比较，如果相等，计数+1
        if (strncmp(entries[i].ts, today, 10) == 0) cnt++;
    }
    free(entries); // 释放通过查询申请的堆内存
    return cnt;
}

/* =========================================================================
 * 异步后台统计逻辑（核心难点）
 * 定时器只做极其快速的内存查询和UI刷新（不卡顿）。
 * 复杂的读文件、JSON解析、日志检索统统打包交给后台 Worker 线程。
 * ========================================================================= */

/* 线程间传递数据的“快递盒”结构体 */
typedef struct {
    int  user_count;     // 保存后台查出来的用户数
    int  event_count;    // 保存后台查出来的今日事件数
    char last_open[48];  // 保存后台查出来的最后开启时间字符串
} monitor_stats_t;

/* 
 * 节流阀 / 防重叠标志位：
 * 因为读写 U盘/Flash 可能很慢。假如定时器1秒触发一次，而后台查日志需要2秒。
 * 如果没有这个标志位，定时器就会疯狂向后台投递任务，导致任务队列大爆炸。
 */
static bool s_stats_busy = false;   

/**
 * @brief 【在后台线程中执行】的耗时函数。
 * 绝对不能在这里直接修改任何 LVGL UI 控件，会引发崩溃！
 */
static void stats_worker(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    
    /* 1. 耗时操作：从文件读取所有用户 JSON 并解析，获取用户数 */
    safe_user_t * us = NULL;
    int uc = 0;
    if (user_load_all(&us, &uc) == 0) { 
        a->user_count = uc; 
        user_list_free(us); // 拿到数量后，链表实体没用了，释放掉
    }

    /* 2. 耗时操作：检索文件日志，计算今日事件数 */
    a->event_count = count_today_events();

    /* 3. 耗时操作：再次检索日志，找出最近 1 条（数量为1）包含 "unlock" 的记录 */
    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query("unlock", 1, &entries, &n) == 0 && n > 0) {
        // 提取 "YYYY-MM-DD HH:MM:SS" 中的 "MM-DD HH:MM" 
        // entries[0].ts + 5 代表跳过 "YYYY-"
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：%.5s %.5s",
                 entries[0].ts + 5, entries[0].ts + 11);
        free(entries);
    } else {
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：--");
    }
    // 执行到这里，后台计算全部完成。数据已经填满了包裹（包裹指针 a）。
}

/**
 * @brief 【在主线程（UI线程）中执行】的回调函数。
 * 当 Worker 线程完成上面的任务后，主线程（通过 worker_poll）会调用这里。
 */
static void stats_done(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    
    // 任务完成，解除节流锁，允许定时器投递下一轮后台任务
    s_stats_busy = false;

    // 因为当前运行在主线程，可以极其安全地更新 LVGL 的各种标签控件了
    char buf[16];
    
    snprintf(buf, sizeof(buf), "%d", a->user_count);
    lv_label_set_text(s_user_count, buf);
    
    snprintf(buf, sizeof(buf), "%d", a->event_count);
    lv_label_set_text(s_event_count, buf);
    
    lv_label_set_text(s_net_state, "已连接");
    
    lv_label_set_text(s_last_label, a->last_open);

    // 释放那个装满数据的包裹（包裹是在定时器里 malloc 的）
    free(a);
}

/**
 * @brief 周期定时器回调 (1000ms触发一次)
 * 运行在主线程。要求：内部代码执行必须快如闪电，绝对不能有阻塞等待。
 */
static void monitor_timer_cb(lv_timer_t * t)
{
    (void)t;

    /* 第一部分：纯内存级极速判断与 UI 刷新 */
    // actuator_get_state() 通常只是读取一个 GPIO 电平或一个内存变量，耗时纳秒级
    bool open = actuator_get_state();
    
    // 根据状态修改大圆环的边框颜色、文字颜色、文字内容和主标题
    lv_obj_set_style_border_color(s_lock_ring, open ? theme_color(TH_OK) : theme_color(TH_ACCENT), 0);
    lv_obj_set_style_text_color(s_lock_icon, open ? theme_color(TH_OK) : theme_color(TH_ACCENT), 0);
    lv_label_set_text(s_lock_icon, open ? "开" : "锁");
    lv_label_set_text(s_status_label, open ? "保险柜已开启" : "保险柜已上锁");

    /* 第二部分：投递复杂的统计逻辑给后台线程 */
    
    // 如果上一轮后台任务还没做完（可能是 Flash 正在被大量写入导致读取慢），直接返回，不堆积任务
    if (s_stats_busy) return;
    
    // 给接下来的任务上锁
    s_stats_busy = true;
    
    // 动态申请一个“快递盒”内存，用于给后台送数据、接收结果
    monitor_stats_t * a = (monitor_stats_t *)malloc(sizeof(*a));
    if (!a) { 
        // 内存不足时，解锁并退出，等下一秒再试
        s_stats_busy = false; 
        return; 
    }
    memset(a, 0, sizeof(*a)); // 清空包裹
    
    // 把任务放进 Worker 队列
    // 参数1：后台任务(耗时)；参数2：包裹指针；参数3：主线程结束回调(安全刷新UI)
    worker_post(stats_worker, a, stats_done);
}