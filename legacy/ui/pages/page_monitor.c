/**
 * @file page_monitor.c
 * 主页（v2 设计）：中央锁状态圆环（缩小到 140px）+ 双开锁方式（密码/人脸）+ 底部紧凑信息条。
 *
 * 设计目标（DESIGN.md v2）：
 *   1) 解决原版"开锁按钮被底栏挡住"问题：内容总高必须 < 内容区 (h - top - tab)；
 *   2) 双开锁方式左右并排：密码（实装） / 人脸（占位，"未配置"灰显）；
 *   3) 信息条三联横排紧凑显示，节省垂直空间。
 *
 * 从上到下布局：
 *   1) 状态主标题 + 最后一次开启时间（紧凑组合）
 *   2) 中央锁状态圆环（140px 描边环）
 *   3) 双开锁方式：密码开锁 / 人脸开锁（占位）
 *   4) 底部三联信息条：用户 | 今日事件 | 网络
 */
#include "page_monitor.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"       /* SX/SY 自适应缩放 */
#include "core/store.h"
#include "core/worker.h"
#include "core/auth_fsm.h"
#include "hal/actuator.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================== 全局静态 UI 对象指针 ================== */
static lv_obj_t * s_lock_ring;      // 中央的大圆环
static lv_obj_t * s_lock_icon;      // 圆环中间的字（"锁"/"开"）
static lv_obj_t * s_status_label;   // 状态主标题（"保险柜已上锁"等）
static lv_obj_t * s_last_label;     // 最后一次开启时间文本
static lv_obj_t * s_user_count;     // 底部卡片1：用户数量的数值文本
static lv_obj_t * s_event_count;    // 底部卡片2：今日事件数量的数值文本
static lv_obj_t * s_net_state;      // 底部卡片3：网络状态的文本

/* ================== 内部私有函数声明 ================== */
static void monitor_timer_cb(lv_timer_t * t);
static void build_info_card(lv_obj_t * parent, const char * title, lv_obj_t ** value_lbl);
static int count_today_events(void);
static void unlock_go_cb(lv_event_t * e);   /* 主页"开锁"按钮 → PIN 页 */
static void face_unlock_cb(lv_event_t * e); /* 主页"人脸开锁"模拟按钮 */
/* 主题切换回调：定义在文件后半部，需前置声明 */
static void monitor_refresh_theme(int idx);

/**
 * @brief 创建监控主页面（由 UI 框架调用）
 */
lv_obj_t * page_monitor_create(lv_obj_t * parent)
{
    /* 1. 创建页面的根容器 root */
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    
    /* 设置内边距 —— 缩放 */
    lv_obj_set_style_pad_all(root, SX(16), 0);
    lv_obj_set_style_pad_row(root, SY(20), 0);

    /* 弹性盒子布局 */
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN); 
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 2. 中央锁状态大圆环 —— ★ 修复：原 SX(200) 过大，1.8x 缩放下总内容高度超内容区，
     *      导致"开锁"按钮被底栏压住。改 SX(140)，描边 SY(5)，字 20pt */
    s_lock_ring = lv_obj_create(root);
    lv_obj_set_size(s_lock_ring, SX(140), SX(140));
    lv_obj_set_style_radius(s_lock_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_lock_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lock_ring, SY(5), 0);
    lv_obj_set_style_border_color(s_lock_ring, theme_color(TH_ACCENT), 0);

    lv_obj_set_flex_flow(s_lock_ring, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lock_ring, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lock_icon = lv_label_create(s_lock_ring);
    lv_label_set_text(s_lock_icon, "锁");
    lv_obj_add_style(s_lock_icon, &st_text, 0);
    lv_obj_set_style_text_font(s_lock_icon, app_font_scaled(20), 0);
    lv_obj_set_style_text_color(s_lock_icon, theme_color(TH_ACCENT), 0);

    /* 3. 状态主标题 + 最后一次开启时间（紧凑组合，pad_row 减到 12） */
    lv_obj_set_style_pad_row(root, SY(12), 0);
    s_status_label = lv_label_create(root);
    lv_label_set_text(s_status_label, "保险柜已上锁");
    lv_obj_add_style(s_status_label, &st_text, 0);
    lv_obj_set_style_text_font(s_status_label, app_font_scaled(20), 0);

    s_last_label = lv_label_create(root);
    lv_label_set_text(s_last_label, "最后一次开启：--");
    lv_obj_add_style(s_last_label, &st_text_mut, 0);
    lv_obj_set_style_text_font(s_last_label, app_font_scaled(13), 0);

    /* 4. ★ v2 双开锁方式：左 密码开锁（实装）/ 右 人脸开锁（占位） */
    lv_obj_t * methods = lv_obj_create(root);
    lv_obj_set_size(methods, lv_pct(100), SY(72));
    lv_obj_set_style_bg_opa(methods, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(methods, 0, 0);
    lv_obj_set_style_pad_all(methods, 0, 0);
    lv_obj_set_style_pad_column(methods, SX(16), 0);
    lv_obj_set_flex_flow(methods, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(methods, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 密码开锁（accent 色，实装） */
    lv_obj_t * pin_btn = lv_button_create(methods);
    lv_obj_set_flex_grow(pin_btn, 1);
    lv_obj_set_height(pin_btn, lv_pct(100));
    lv_obj_add_style(pin_btn, &st_accent_btn, 0);
    lv_obj_add_style(pin_btn, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_style_radius(pin_btn, SX(12), 0);
    lv_obj_set_flex_flow(pin_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pin_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(pin_btn, unlock_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pin_icon = lv_label_create(pin_btn);
    lv_label_set_text(pin_icon, "密");  /* 密（Noto CJK 一定有，emoji 不一定有） */
    lv_obj_set_style_text_font(pin_icon, app_font_scaled(22), 0);
    lv_obj_set_style_text_color(pin_icon, theme_color(TH_ACCENT_INK), 0);
    lv_obj_t * pin_txt = lv_label_create(pin_btn);
    lv_label_set_text(pin_txt, "密码开锁");
    lv_obj_set_style_text_font(pin_txt, app_font_scaled(14), 0);
    lv_obj_set_style_text_color(pin_txt, theme_color(TH_ACCENT_INK), 0);
    lv_obj_set_style_pad_top(pin_txt, SY(4), 0);

    /* 人脸开锁（ghost 灰显，DESIGN.md 预留 / TODO：接人脸识别后端） */
    lv_obj_t * face_btn = lv_button_create(methods);
    lv_obj_set_flex_grow(face_btn, 1);
    lv_obj_set_height(face_btn, lv_pct(100));
    lv_obj_add_style(face_btn, &st_ghost_btn, 0);
    lv_obj_set_style_radius(face_btn, SX(12), 0);
    lv_obj_set_flex_flow(face_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(face_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(face_btn, face_unlock_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * face_icon = lv_label_create(face_btn);
    lv_label_set_text(face_icon, "脸");  /* 脸 */
    lv_obj_set_style_text_font(face_icon, app_font_scaled(22), 0);
    lv_obj_t * face_txt = lv_label_create(face_btn);
    lv_label_set_text(face_txt, "人脸开锁");
    lv_obj_set_style_text_font(face_txt, app_font_scaled(14), 0);
    lv_obj_set_style_pad_top(face_txt, SY(4), 0);
    /* "模拟" 角标（阶段 1 PC mock：UI 注入置信度，阶段 3 接真实 FM225） */
    lv_obj_t * face_tag = lv_label_create(face_btn);
    lv_label_set_text(face_tag, "模拟");
    lv_obj_set_style_text_font(face_tag, app_font_scaled(10), 0);
    lv_obj_set_style_text_color(face_tag, theme_color(TH_TEXT_MUT), 0);
    lv_obj_align(face_tag, LV_ALIGN_TOP_RIGHT, -SX(6), SY(4));

    /* 5. 底部三联信息条（紧凑横排） */
    lv_obj_t * cards = lv_obj_create(root);
    /* 卡片内部高度 SY(96)+padding，容器高度必须 ≥ 子卡片高度，否则标题会被裁剪 */
    lv_obj_set_size(cards, lv_pct(100), SY(120));
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_set_style_pad_column(cards, SX(12), 0);

    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_info_card(cards, "用户", &s_user_count);
    build_info_card(cards, "今日事件", &s_event_count);
    build_info_card(cards, "网络", &s_net_state);

    /* 7. 定时器 */
    lv_timer_create(monitor_timer_cb, 1000, root);
    monitor_timer_cb(NULL);

    /* 8. 注册主题切换回调：让主页大圆环/锁字/开锁按钮的本地颜色覆盖也随主题刷新 */
    theme_register_change_cb(monitor_refresh_theme);

    return root;
}

/**
 * @brief 辅助函数：构造一个数据信息卡片（尺寸全部缩放）
 */
static void build_info_card(lv_obj_t * parent, const char * title, lv_obj_t ** value_lbl)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, SX(160), SY(96));           /* 卡片尺寸缩放 */
    
    lv_obj_set_flex_grow(card, 1); 
    lv_obj_add_style(card, &st_panel, 0);
    lv_obj_set_style_pad_all(card, SX(14), 0);         /* 内边距缩放 */
    
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 标题
    lv_obj_t * t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_add_style(t, &st_text_mut, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(14), 0);

    // 数值
    *value_lbl = lv_label_create(card);
    lv_label_set_text(*value_lbl, "--");
    lv_obj_add_style(*value_lbl, &st_text, 0);
    lv_obj_set_style_text_font(*value_lbl, app_font_scaled(28), 0);
    lv_obj_set_style_pad_top(*value_lbl, SY(6), 0);    /* 间距缩放 */
}

/**
 * @brief 业务函数：统计今天的日志事件数量
 */
static int count_today_events(void)
{
    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query(NULL, -1, &entries, &n) != 0) return 0;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);

    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(entries[i].ts, today, 10) == 0) cnt++;
    }
    free(entries);
    return cnt;
}

/* =========================================================================
 * 异步后台统计逻辑
 * ========================================================================= */

typedef struct {
    int  user_count;
    int  event_count;
    char last_open[48];
} monitor_stats_t;

static bool s_stats_busy = false;   

static void stats_worker(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    
    safe_user_t * us = NULL;
    int uc = 0;
    if (user_load_all(&us, &uc) == 0) { 
        a->user_count = uc; 
        user_list_free(us);
    }

    a->event_count = count_today_events();

    log_entry_t * entries = NULL;
    int n = 0;
    if (log_query("unlock", 1, &entries, &n) == 0 && n > 0) {
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：%.5s %.5s",
                 entries[0].ts + 5, entries[0].ts + 11);
        free(entries);
    } else {
        snprintf(a->last_open, sizeof(a->last_open), "最后一次开启：--");
    }
}

static void stats_done(void * p)
{
    monitor_stats_t * a = (monitor_stats_t *)p;
    
    s_stats_busy = false;

    char buf[16];
    
    snprintf(buf, sizeof(buf), "%d", a->user_count);
    lv_label_set_text(s_user_count, buf);
    
    snprintf(buf, sizeof(buf), "%d", a->event_count);
    lv_label_set_text(s_event_count, buf);
    
    lv_label_set_text(s_net_state, "已连接");
    
    lv_label_set_text(s_last_label, a->last_open);

    free(a);
}

/* 把"根据锁状态刷新控件颜色"独立出来：既被 timer 调用，也被主题切换回调调用 */
static void monitor_refresh_state_colors(void)
{
    bool open = actuator_get_state();
    if (s_lock_ring) lv_obj_set_style_border_color(s_lock_ring, open ? theme_color(TH_OK) : theme_color(TH_ACCENT), 0);
    if (s_lock_icon) {
        lv_obj_set_style_text_color(s_lock_icon, open ? theme_color(TH_OK) : theme_color(TH_ACCENT), 0);
        lv_label_set_text(s_lock_icon, open ? "开" : "锁");
    }
    if (s_status_label) lv_label_set_text(s_status_label, open ? "保险柜已开启" : "保险柜已上锁");
}

/* 主题切换回调：让主页大圆环/锁字/开锁按钮的本地颜色覆盖也随主题刷新 */
static void monitor_refresh_theme(int idx)
{
    (void)idx;
    monitor_refresh_state_colors();
}

static void monitor_timer_cb(lv_timer_t * t)
{
    (void)t;
    monitor_refresh_state_colors();

    if (s_stats_busy) return;
    
    s_stats_busy = true;
    
    monitor_stats_t * a = (monitor_stats_t *)malloc(sizeof(*a));
    if (!a) { 
        s_stats_busy = false; 
        return; 
    }
    memset(a, 0, sizeof(*a));
    
    worker_post(stats_worker, a, stats_done);
}

/* 主页"开锁"按钮回调：跳转到 PIN 键盘（解锁）页 */
static void unlock_go_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_KEYPAD);
}

/* 阶段 1：人脸开锁（PC 端以「注入一次高置信度识别」模拟 FM225 命中 admin）。
 * 真实模组阶段 3 接入后，这里改为触发 hal_face 轮询即可，业务不动（NFR-5）。 */
static void face_unlock_cb(lv_event_t * e)
{
    (void)e;
    /* admin 在 bootstrap 中预置 face_id=1、score_high=85：
     * 注入 90 ≥ 85 → 直接开锁；改 75 走动态码；改 50 拒绝（用 MQTT inject_score 调试）。 */
    auth_fsm_submit_detect(1, 90);
}











