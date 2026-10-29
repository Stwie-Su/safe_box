/**
 * @file icons.h
 * 图标：两种做法都在这里。
 *
 *   1) 矢量拼装（ui_icon_create）：用矩形、圆弧、文字块拼出一个图标。
 *      适合少数形状复杂的图标，颜色能随主题实时变，不占额外显存。
 *   2) 图标字体（icon_label）：把 Material Icons 里常用的一批图标编进固件，
 *      当文字用。界面里绝大多数图标走这条，字号有 16/20/24/32 四档。
 *
 * 规矩：颜色一律用主题里的角色（TH_ACCENT / TH_TEXT ...），不要传死值，
 * 否则换主题时图标不会跟着变。
 */
#pragma once
#include "lvgl.h"
#include "ui/theme.h"   /* theme_role_t / theme_color() */

#ifdef __cplusplus
extern "C" {
#endif

/* MAX/MIN：有些平台的头文件不提供，这里自己补上。 */
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* 矢量图标的种类。 */
typedef enum {
    UI_ICON_LOCK        = 0,   /* 圆环 + 锁（主页中心） */
    UI_ICON_KEYPAD,            /* 九宫格（密码开锁卡） */
    UI_ICON_FACE_SCAN,         /* 四角扫描框 + 人脸（人脸识别卡） */
    UI_ICON_USER,              /* 头像（用户卡） */
    UI_ICON_LIST,              /* 三横线 + 点（事件卡） */
    UI_ICON_LIST_TAB,          /* 三横线（底部日志页签） */
    UI_ICON_CALENDAR,          /* 日历（暂时没用到） */
    UI_ICON_SIGNAL,            /* 信号条（网络卡） */
    UI_ICON_HOME,              /* 房子（底部主页页签） */
    UI_ICON_SETTINGS,          /* 齿轮（底部设置页签） */
    UI_ICON_BACK,              /* 左箭头（返回） */
    UI_ICON_CLOSE,             /* × 关闭 */
    UI_ICON_KEY_LOCKED,        /* 小锁（角标） */
    UI_ICON_COUNT
} ui_icon_kind_t;

/* 图标字体的码位。加新图标时：字体文件里加上这个码位，这里也加一项，
 * 两边必须一致（icon_font_selfcheck 会逐个数检查，缺了会打印 MISS）。 */
typedef enum {
    UI_GLYPH_HOME      = 0xE88A,  /* 主页 */
    UI_GLYPH_LOCK      = 0xE897,  /* 上锁 */
    UI_GLYPH_LOCK_OPEN = 0xE898,  /* 开锁 */
    UI_GLYPH_PERSON    = 0xE7FD,  /* 用户 */
    UI_GLYPH_GROUP     = 0xE7EF,  /* 用户组 */
    UI_GLYPH_LIST      = 0xEF6E,  /* 日志 */
    UI_GLYPH_FACE      = 0xE87C,  /* 人脸 */
    UI_GLYPH_WIFI      = 0xE63E,  /* 网络 */
    UI_GLYPH_SETTINGS  = 0xE8B8,  /* 设置 */
    UI_GLYPH_SYSTEM    = 0xE322,  /* 系统 */
    UI_GLYPH_FINGER    = 0xE90D,  /* 指纹 */
    UI_GLYPH_BELL      = 0xE7F4,  /* 提醒 */
    UI_GLYPH_KEY       = 0xE0DA,  /* 密钥 */
    UI_GLYPH_PLUS      = 0xE145,  /* 添加 */
    UI_GLYPH_CLOSE     = 0xE5CD,  /* 关闭 */
    UI_GLYPH_BACK      = 0xE5C4,  /* 返回 */
    UI_GLYPH_CHEVRON_R = 0xE5CC,  /* 右箭头 */
    UI_GLYPH_SHIELD    = 0xE9E0,  /* 安全 */
    UI_GLYPH_CAMERA    = 0xE412,  /* 摄像头 */
    UI_GLYPH_BATTERY   = 0xE1A4,  /* 电池 */
    UI_GLYPH_POWER     = 0xE8AC,  /* 电源 */
    UI_GLYPH_OK        = 0xE86C,  /* 成功（勾） */
    UI_GLYPH_WARN      = 0xE002,  /* 警告 */
    UI_GLYPH_DANGER    = 0xE000,  /* 失败 */
    UI_GLYPH_TIME      = 0xE8B5,  /* 时间 */
    UI_GLYPH_DATE      = 0xE916,  /* 日期 */
    UI_GLYPH_STORAGE   = 0xE1C2,  /* 存储 */
    UI_GLYPH_UNDO      = 0xE863,  /* 撤销 / 恢复出厂 */
    UI_GLYPH_DELETE    = 0xE872,  /* 删除 */
    UI_GLYPH_EDIT      = 0xE3C9,  /* 编辑 */
    UI_GLYPH_EYE       = 0xE8F4,  /* 查看 */
    UI_GLYPH_BOLT      = 0xE3E7,  /* 电量 */
    UI_GLYPH_DONE      = 0xE876,  /* 完成 */
    UI_GLYPH_SEARCH    = 0xE8B6,  /* 搜索 */
    UI_GLYPH_LOGOUT    = 0xE879,  /* 退出 */
    UI_GLYPH_TOGGLE_ON = 0xE9F6,  /* 开关（开） */
    UI_GLYPH_TOGGLE_OFF= 0xE9F5,  /* 开关（关） */
    UI_GLYPH_QR        = 0xEF6B,  /* 二维码 / 动态码 */
    UI_GLYPH_LAN       = 0xE8BE,  /* 有线网络 */
    UI_GLYPH_PIN       = 0xE0BC,  /* PIN */
    UI_GLYPH_COUNT
} ui_glyph_t;

/* 建一个矢量图标。size 是正方形边长，调用方要先按屏幕比例缩放。 */
lv_obj_t * ui_icon_create(lv_obj_t * parent,
                          ui_icon_kind_t kind,
                          int32_t size,
                          lv_color_t color);

/* 给矢量图标换颜色（主题切换时批量刷新用）。 */
void ui_icon_set_color(lv_obj_t * icon, lv_color_t color);

/* ---------------- 图标字体 ---------------- */

/* 按设计稿像素选最接近的一档图标字体（16/20/24/32，往大取）。 */
const lv_font_t * ui_icon_font(int32_t px);

/* 建一个图标文字。role 是主题颜色角色，不要传死颜色。 */
lv_obj_t * icon_label_colored(lv_obj_t * parent, ui_glyph_t glyph,
                              int32_t size, theme_role_t role);

/* 同上，颜色用正文色。 */
lv_obj_t * icon_label(lv_obj_t * parent, ui_glyph_t glyph, int32_t size);

/* 运行时换一个图标（不重建控件）。例如锁状态图标从「锁」变「开锁」。 */
void icon_label_set_glyph(lv_obj_t * lb, ui_glyph_t glyph);

/* 自检：把每个码位、每档字号都查一遍，缺字形就打印 MISS。
 * 设了 SAFE_ICON_CHECK=1 时 ui_init() 会调一次，换字体后用它确认没漏。 */
void icon_font_selfcheck(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
