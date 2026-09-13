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
 *
 * UI 重设计（ui4）新增**图标字体**通道：
 *  - ui_icon_create() 是「矢量几何拼装」，适合少数复杂图形；
 *  - icon_label() 走编译进固件的图标字体（Material Icons 子集），
 *    导航/卡片/列表行/按钮统一用它，码位见 ui_glyph_t，颜色一律取 theme.c token，
 *    四套主题自动跟随（lv_label 的 text_color = token 色）。
 */
#pragma once
#include "lvgl.h"
#include "ui/theme.h"   /* theme_role_t / theme_color()：图标上色只走 token */

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
 * @brief 图标字体的码位（Material Icons 子集，PUA 区）。
 * 生成：见 app/ui/fonts/lv_font_icon_*.c 文件头注释；四套字号 16/20/24/32。
 * 新增图标：在 lv_font_conv 的 -r 码位表里加，再在此枚举补一项，二者必须一致
 * （icon_font_selfcheck() 会逐档逐码位校验，缺字形会打印 MISS）。
 */
typedef enum {
    UI_GLYPH_HOME      = 0xE88A,  /* 主页 */
    UI_GLYPH_LOCK      = 0xE897,  /* 上锁 */
    UI_GLYPH_LOCK_OPEN = 0xE898,  /* 开锁 */
    UI_GLYPH_PERSON    = 0xE7FD,  /* 用户 */
    UI_GLYPH_GROUP     = 0xE7EF,  /* 用户组 */
    UI_GLYPH_LIST      = 0xEF6E,  /* 日志（清单） */
    UI_GLYPH_FACE      = 0xE87C,  /* 人脸 */
    UI_GLYPH_WIFI      = 0xE63E,  /* 网络 */
    UI_GLYPH_SETTINGS  = 0xE8B8,  /* 设置 */
    UI_GLYPH_SYSTEM    = 0xE322,  /* 系统（板卡） */
    UI_GLYPH_FINGER    = 0xE90D,  /* 指纹（一键开锁） */
    UI_GLYPH_BELL      = 0xE7F4,  /* 事件 / 提醒 */
    UI_GLYPH_KEY       = 0xE0DA,  /* 密钥 / 存储占用 */
    UI_GLYPH_PLUS      = 0xE145,  /* 添加 */
    UI_GLYPH_CLOSE     = 0xE5CD,  /* 关闭 */
    UI_GLYPH_BACK      = 0xE5C4,  /* 返回 / 左箭头 */
    UI_GLYPH_CHEVRON_R = 0xE5CC,  /* 右箭头（列表行末） */
    UI_GLYPH_SHIELD    = 0xE9E0,  /* 安全 / 防护 */
    UI_GLYPH_CAMERA    = 0xE412,  /* 摄像头 */
    UI_GLYPH_BATTERY   = 0xE1A4,  /* 电池 */
    UI_GLYPH_POWER     = 0xE8AC,  /* 电源 / 重启 */
    UI_GLYPH_OK        = 0xE86C,  /* 成功（勾圆） */
    UI_GLYPH_WARN      = 0xE002,  /* 警示 */
    UI_GLYPH_DANGER    = 0xE000,  /* 危险 / 失败 */
    UI_GLYPH_TIME      = 0xE8B5,  /* 时间 */
    UI_GLYPH_DATE      = 0xE916,  /* 日期 */
    UI_GLYPH_STORAGE   = 0xE1C2,  /* 存储 */
    UI_GLYPH_UNDO      = 0xE863,  /* 撤销 / 恢复出厂 */
    UI_GLYPH_DELETE    = 0xE872,  /* 删除 */
    UI_GLYPH_EDIT      = 0xE3C9,  /* 编辑 */
    UI_GLYPH_EYE       = 0xE8F4,  /* 可见 / 查看 */
    UI_GLYPH_BOLT      = 0xE3E7,  /* 电量 / 快速 */
    UI_GLYPH_DONE      = 0xE876,  /* 完成 */
    UI_GLYPH_SEARCH    = 0xE8B6,  /* 搜索 */
    UI_GLYPH_LOGOUT    = 0xE879,  /* 登出 */
    UI_GLYPH_TOGGLE_ON = 0xE9F6,  /* 开关（开） */
    UI_GLYPH_TOGGLE_OFF= 0xE9F5,  /* 开关（关） */
    UI_GLYPH_QR        = 0xEF6B,  /* 二维码 / 动态码 */
    UI_GLYPH_LAN       = 0xE8BE,  /* 以太网 */
    UI_GLYPH_PIN       = 0xE0BC,  /* PIN 码 */
    UI_GLYPH_COUNT
} ui_glyph_t;

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

/* ================================================================
 *  图标字体通道（UI 重设计 ui4）
 * ================================================================ */

/**
 * @brief 按设计稿像素挑选图标字号的 lv_font_t（16/20/24/32 四档，向上取最近档）。
 */
const lv_font_t * ui_icon_font(int32_t px);

/**
 * @brief 图标字体 label：内部按码位拼 UTF-8、挂图标字体、按 token 上色。
 * @param parent 父对象
 * @param glyph  码位（ui_glyph_t）
 * @param size   设计稿像素（会选到 16/20/24/32 中最近档）
 * @param role   颜色 token（TH_TEXT / TH_ACCENT / TH_OK ...），禁止传 hex
 * @return lv_label 对象（可直接 set_pos / 挂事件）
 */
lv_obj_t * icon_label_colored(lv_obj_t * parent, ui_glyph_t glyph,
                              int32_t size, theme_role_t role);

/**
 * @brief icon_label_colored 的默认色版本（正文色 TH_TEXT）。
 */
lv_obj_t * icon_label(lv_obj_t * parent, ui_glyph_t glyph, int32_t size);

/**
 * @brief 图标字体自检：逐个码位 × 每个字号调 lv_font_get_glyph_dsc() 查字形，
 * 缺失打印 MISS。SAFE_ICON_CHECK=1 时由 ui_init() 调用一次，用于换字体后自证。
 */
void icon_font_selfcheck(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

