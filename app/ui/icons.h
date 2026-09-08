/**
 * @file icons.h
 * 通用矢量图标库：所有图标以「透明容器 + 几何组合」方式构建，便于换色与缩放，
 * 完全替代原先用单字「锁/密/脸/用」等中文字符当图标的折中方案。
 *
 * 设计要点：
 *  - 图标以透明 lv_obj 作为「图标盒子」，内嵌 lv_obj 矩形/lv_arc/lv_label；
 *  - 不使用 lv_canvas：避免额外 framebuffer 占用，颜色随主题实时变化；
 *  - 图标颜色由调用方传入（典型用法：theme_color(TH_ACCENT)），切主题时由
 *    theme_register_change_cb 中调 ui_icon_set_color() 批量刷新；
 *  - 图标尺寸由调用方传入 SX()/SY()，外部已做 1024×600→多分辨率缩放；
 *  - 所有图标非 clickable（吃掉事件），父级卡片负责接收点击。
 */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MAX/MIN（标准 libcsys/param.h 引入，但在 desktop libc 上不一定暴露，自定义） */
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef enum {
    UI_ICON_LOCK        = 0,   /* 圆环包住 + 锁头（主页中心、上锁/已锁用） */
    UI_ICON_KEYPAD,            /* 4×3 九宫格点（密码开锁卡） */
    UI_ICON_FACE_SCAN,         /* 4 角扫描框 + 中央人脸位（人脸识别卡/页面） */
    UI_ICON_USER,              /* 圆头 + 肩部弧（用户卡） */
    UI_ICON_LIST,              /* 三横线 + 圆点（今日事件卡） */
    UI_ICON_LIST_TAB,          /* 三横线（底部 Tab 日志） */
    UI_ICON_CALENDAR,          /* 日历（备用，当前主页未使用） */
    UI_ICON_SIGNAL,            /* 4 根递增竖条（网络卡） */
    UI_ICON_HOME,              /* 房子（底部 Tab 主页） */
    UI_ICON_SETTINGS,          /* 8 辐齿轮（底部 Tab 设置） */
    UI_ICON_BACK,              /* 左箭头（页面返回） */
    UI_ICON_CLOSE,             /* × 关闭 */
    UI_ICON_KEY_LOCKED,        /* 小锁头（人脸卡角标等） */
    UI_ICON_COUNT
} ui_icon_kind_t;

/**
 * @brief 创建一个矢量图标（透明容器，内部画好矢量图形）
 * @param parent    父对象
 * @param kind      图标类型（见 ui_icon_kind_t）
 * @param size      外框尺寸（正方形），调用方应已传入 SX/SY 缩放后的值
 * @param color     图标主色（描边/填充）
 * @return 图标容器本身（可以直接 lv_obj_set_pos 摆放）
 */
lv_obj_t * ui_icon_create(lv_obj_t * parent,
                          ui_icon_kind_t kind,
                          int32_t size,
                          lv_color_t color);

/**
 * @brief 运行时改色：遍历图标内所有染色对象，重新设置 color。
 * 主题切换回调（topbar_refresh_theme / monitor_refresh_theme）中调用，保证换主题时图标跟变。
 */
void ui_icon_set_color(lv_obj_t * icon, lv_color_t color);

#ifdef __cplusplus
} /*extern "C"*/
#endif
