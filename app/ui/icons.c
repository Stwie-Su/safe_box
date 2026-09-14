/**
 * @file icons.c
 * 通用矢量图标实现。所有几何元素统一带 UID_MAGIC 标记，
 * 主题切换回调可通过 ui_icon_set_color() 遍历子对象一次性换色。
 */
#include "icons.h"
#include "ui/theme.h"                 /* 图标上色只走 token */
#include "ui/fonts/lv_font_icon.h"   /* 编译进位图图标字体 */
#include <stdio.h>                   /* 自检打印 */
#include <stddef.h>

#define UID_MAGIC     0x49434E31u     /* "ICN1" — 标识我们创建的对象 */

/* 子对象 / 图标根上的 user_data（颜色由调用方提供，set_color 时批量更新） */
typedef struct {
    uint32_t magic;
    lv_color_t color;
} uid_t;

/* 创建一个「填充矩形 + outline 描边」的子对象 */
static lv_obj_t * mk_rect(lv_obj_t * parent, int32_t x, int32_t y,
                          int32_t w, int32_t h, int32_t radius,
                          lv_color_t color, bool filled)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, MAX(w / 12, 1), 0);
    lv_obj_set_style_outline_color(o, color, 0);
    lv_obj_set_style_outline_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
    u->magic = UID_MAGIC; u->color = color;
    lv_obj_set_user_data(o, u);
    return o;
}

/* 在指定中心位置画一个填充圆点 */
static lv_obj_t * mk_dot(lv_obj_t * parent, int32_t cx, int32_t cy,
                         int32_t r, lv_color_t color)
{
    lv_obj_t * o = lv_obj_create(parent);
    int32_t d = r * 2;
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
    u->magic = UID_MAGIC; u->color = color;
    lv_obj_set_user_data(o, u);
    return o;
}

/* 画一条带旋转的细长线段（用细矩形旋转） */
static lv_obj_t * mk_line(lv_obj_t * parent, int32_t x, int32_t y,
                          int32_t w, int32_t h, int32_t angle,
                          lv_color_t color)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, MAX(w, h) / 2, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    if (angle != 0) {
        lv_obj_set_style_transform_angle(o, angle * 10, 0);
        lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
    }
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
    u->magic = UID_MAGIC; u->color = color;
    lv_obj_set_user_data(o, u);
    return o;
}

/* 给图标根容器打 tag */
static void tag_root(lv_obj_t * icon)
{
    uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
    u->magic = UID_MAGIC; u->color = lv_color_white();
    lv_obj_set_user_data(icon, u);
}

/* lvgl 9 的树遍历回调签名：返回 lv_obj_tree_walk_res_t */
static lv_obj_tree_walk_res_t recolor_tree(lv_obj_t * obj, void * ctx)
{
    uid_t * u = (uid_t *)lv_obj_get_user_data(obj);
    if (!u || u->magic != UID_MAGIC) return LV_OBJ_TREE_WALK_NEXT;
    lv_color_t new_col = *(lv_color_t *)ctx;
    u->color = new_col;
    lv_obj_set_style_text_color(obj, new_col, 0);
    lv_obj_set_style_bg_color(obj, new_col, 0);
    lv_obj_set_style_outline_color(obj, new_col, 0);
    return LV_OBJ_TREE_WALK_NEXT;
}

/* ----- 各图标实现 ----- */

/* ICON_LOCK：圆环包住的锁头（外环 + 锁体 + shackle + 锁孔） */
static void mk_lock(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t ring_d = (int32_t)(s * 0.86);
    lv_obj_t * ring = lv_obj_create(icn);
    lv_obj_set_size(ring, ring_d, ring_d);
    lv_obj_set_pos(ring, (s - ring_d) / 2, (s - ring_d) / 2);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_outline_width(ring, MAX(s / 16, 3), 0);
    lv_obj_set_style_outline_color(ring, c, 0);
    lv_obj_set_style_outline_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_clickable(ring, false);
    uid_t * u0 = (uid_t *)lv_malloc(sizeof(uid_t));
    u0->magic = UID_MAGIC; u0->color = c;
    lv_obj_set_user_data(ring, u0);

    int32_t body_w = (int32_t)(s * 0.42);
    int32_t body_h = (int32_t)(s * 0.30);
    int32_t body_x = (s - body_w) / 2;
    int32_t body_y = (int32_t)(s * 0.48);
    lv_obj_t * body = lv_obj_create(icn);
    lv_obj_set_pos(body, body_x, body_y);
    lv_obj_set_size(body, body_w, body_h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_outline_width(body, MAX(s / 20, 2), 0);
    lv_obj_set_style_outline_color(body, c, 0);
    lv_obj_set_style_radius(body, s / 16, 0);
    lv_obj_set_clickable(body, false);
    uid_t * u1 = (uid_t *)lv_malloc(sizeof(uid_t));
    u1->magic = UID_MAGIC; u1->color = c;
    lv_obj_set_user_data(body, u1);

    /* shackle — 两根立柱 + 顶部横杠近似 U 形 */
    int32_t post_w = MAX(s / 22, 2);
    int32_t post_h = (int32_t)(s * 0.22);
    int32_t post_y = body_y - post_h + post_w;
    mk_rect(icn, body_x, post_y, post_w, post_h, post_w / 2, c, true);
    mk_rect(icn, body_x + body_w - post_w, post_y, post_w, post_h, post_w / 2, c, true);
    mk_rect(icn, body_x, post_y - post_w / 2,
            body_w, post_w, post_w / 2, c, true);

    /* 锁孔 */
    int32_t kh_r = MAX(s / 24, 3);
    int32_t kh_cx = s / 2;
    int32_t kh_cy = body_y + body_h / 2 - s / 32;
    mk_dot(icn, kh_cx, kh_cy, kh_r, c);
    mk_rect(icn, kh_cx - kh_r / 2, kh_cy + kh_r / 2,
            kh_r, body_h / 3, kh_r / 2, c, true);
}

/* ICON_KEYPAD：3 列 × 4 行 圆点格栅 */
static void mk_keypad(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.10);
    int32_t cell_w = (s - 2 * pad) / 3;
    int32_t cell_h = (s - 2 * pad) / 4;
    int32_t dot_r = MIN(cell_w, cell_h) / 4;
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 3; col++) {
            int32_t cx = pad + col * cell_w + cell_w / 2;
            int32_t cy = pad + r * cell_h + cell_h / 2;
            lv_obj_t * d = lv_obj_create(icn);
            int32_t dd = dot_r * 2;
            lv_obj_set_pos(d, cx - dot_r, cy - dot_r);
            lv_obj_set_size(d, dd, dd);
            lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_outline_width(d, MAX(dot_r / 4, 1), 0);
            lv_obj_set_style_outline_color(d, c, 0);
            lv_obj_set_clickable(d, false);
            uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
            u->magic = UID_MAGIC; u->color = c;
            lv_obj_set_user_data(d, u);
        }
    }
}

/* ICON_FACE_SCAN：4 角扫描框 + 中央人头剪影 */
static void mk_face_scan(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t thick = MAX(s / 14, 3);
    int32_t len = (int32_t)(s * 0.22);
    int32_t margin = (int32_t)(s * 0.10);
    int32_t x0 = margin, y0 = margin;
    int32_t x1 = s - margin, y1 = s - margin;

    mk_rect(icn, x0, y0, len, thick, thick / 2, c, true);
    mk_rect(icn, x0, y0, thick, len, thick / 2, c, true);
    mk_rect(icn, x1 - len, y0, len, thick, thick / 2, c, true);
    mk_rect(icn, x1 - thick, y0, thick, len, thick / 2, c, true);
    mk_rect(icn, x0, y1 - thick, len, thick, thick / 2, c, true);
    mk_rect(icn, x0, y1 - len, thick, len, thick / 2, c, true);
    mk_rect(icn, x1 - len, y1 - thick, len, thick, thick / 2, c, true);
    mk_rect(icn, x1 - thick, y1 - len, thick, len, thick / 2, c, true);

    int32_t head_r = (int32_t)(s * 0.13);
    int32_t head_cx = s / 2;
    int32_t head_cy = (int32_t)(s * 0.38);
    lv_obj_t * head = lv_obj_create(icn);
    lv_obj_set_size(head, head_r * 2, head_r * 2);
    lv_obj_set_pos(head, head_cx - head_r, head_cy - head_r);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_outline_width(head, MAX(s / 28, 2), 0);
    lv_obj_set_style_outline_color(head, c, 0);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(head, false);
    uid_t * u0 = (uid_t *)lv_malloc(sizeof(uid_t));
    u0->magic = UID_MAGIC; u0->color = c;
    lv_obj_set_user_data(head, u0);

    int32_t sh_w = (int32_t)(s * 0.40);
    int32_t sh_h = (int32_t)(s * 0.20);
    lv_obj_t * sh = lv_obj_create(icn);
    lv_obj_set_pos(sh, head_cx - sh_w / 2, (int32_t)(s * 0.62));
    lv_obj_set_size(sh, sh_w, sh_h);
    lv_obj_set_style_bg_color(sh, c, 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sh, 0, 0);
    lv_obj_set_style_radius(sh, sh_w / 2, 0);
    lv_obj_set_clickable(sh, false);
    uid_t * u1 = (uid_t *)lv_malloc(sizeof(uid_t));
    u1->magic = UID_MAGIC; u1->color = c;
    lv_obj_set_user_data(sh, u1);
}

/* ICON_USER：人头 + 肩部 */
static void mk_user(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t head_r = (int32_t)(s * 0.20);
    int32_t cx = s / 2;
    int32_t cy = (int32_t)(s * 0.34);
    mk_dot(icn, cx, cy, head_r, c);
    int32_t sh_w = (int32_t)(s * 0.60);
    int32_t sh_h = (int32_t)(s * 0.34);
    lv_obj_t * sh = lv_obj_create(icn);
    lv_obj_set_pos(sh, cx - sh_w / 2, s - sh_h);
    lv_obj_set_size(sh, sh_w, sh_h);
    lv_obj_set_style_bg_color(sh, c, 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sh, 0, 0);
    lv_obj_set_style_radius(sh, sh_w / 2, 0);
    lv_obj_set_clickable(sh, false);
    uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
    u->magic = UID_MAGIC; u->color = c;
    lv_obj_set_user_data(sh, u);
}

/* ICON_LIST：项目符号 + 横线 */
static void mk_list(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.10);
    int32_t row_h = (s - 2 * pad) / 3;
    int32_t dot_r = MAX(row_h / 5, 2);
    int32_t line_w = (int32_t)(s * 0.62);
    int32_t line_h = MAX(s / 26, 2);
    for (int i = 0; i < 3; i++) {
        int32_t cy = pad + i * row_h + row_h / 2;
        mk_dot(icn, pad + dot_r, cy, dot_r, c);
        mk_rect(icn, pad + dot_r * 2 + line_h * 2, cy - line_h / 2,
                line_w, line_h, line_h / 2, c, true);
    }
}

/* ICON_LIST_TAB：底部 Tab 用，简洁 3 条横线 */
static void mk_list_tab(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.16);
    int32_t row_h = (s - 2 * pad) / 3;
    int32_t line_h = MAX(s / 16, 2);
    for (int i = 0; i < 3; i++) {
        int32_t cy = pad + i * row_h + row_h / 2;
        mk_rect(icn, pad, cy - line_h / 2, s - 2 * pad, line_h, line_h / 2, c, true);
    }
}

/* ICON_CALENDAR：外框 + 顶装订 + 3×3 内部网格点 */
static void mk_calendar(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.10);
    int32_t w = s - 2 * pad;
    int32_t h = s - 2 * pad;
    lv_obj_t * box = lv_obj_create(icn);
    lv_obj_set_pos(box, pad, pad);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, MAX(s / 22, 2), 0);
    lv_obj_set_style_outline_color(box, c, 0);
    lv_obj_set_style_radius(box, MAX(s / 18, 4), 0);
    lv_obj_set_clickable(box, false);
    uid_t * ub = (uid_t *)lv_malloc(sizeof(uid_t));
    ub->magic = UID_MAGIC; ub->color = c;
    lv_obj_set_user_data(box, ub);

    mk_rect(icn, pad, pad + h / 5,
            w, MAX(s / 26, 2), s / 26, c, true);

    int32_t inner_x = pad + w / 6;
    int32_t inner_y = pad + h / 2;
    int32_t cell = h / 4;
    int32_t dot_r = MAX(cell / 8, 2);
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int32_t cx = inner_x + col * cell;
            int32_t cy = inner_y + r * cell;
            mk_dot(icn, cx, cy, dot_r, c);
        }
    }
}

/* ICON_SIGNAL：4 根递增信号柱 */
static void mk_signal(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.12);
    int32_t gap = (int32_t)(s * 0.06);
    int32_t n = 4;
    int32_t bar_w = (s - 2 * pad - (n - 1) * gap) / n;
    int32_t max_h = s - 2 * pad;
    for (int i = 0; i < n; i++) {
        int32_t bh = (max_h * (i + 1)) / (n + 1);
        int32_t bx = pad + i * (bar_w + gap);
        int32_t by = s - pad - bh;
        mk_rect(icn, bx, by, bar_w, bh, bar_w / 2, c, true);
    }
}

/* ICON_HOME：房子（顶部斜屋顶 + 方墙 + 门洞） */
static void mk_home(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t pad = (int32_t)(s * 0.10);
    int32_t w = (int32_t)(s * 0.66);
    int32_t h = (int32_t)(s * 0.50);
    int32_t x0 = (s - w) / 2;
    int32_t y0 = s - pad - h;

    /* 主体 outline */
    lv_obj_t * body = lv_obj_create(icn);
    lv_obj_set_pos(body, x0, y0);
    lv_obj_set_size(body, w, h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_outline_width(body, MAX(s / 22, 2), 0);
    lv_obj_set_style_outline_color(body, c, 0);
    lv_obj_set_style_radius(body, MAX(s / 26, 3), 0);
    lv_obj_set_clickable(body, false);
    uid_t * ub = (uid_t *)lv_malloc(sizeof(uid_t));
    ub->magic = UID_MAGIC; ub->color = c;
    lv_obj_set_user_data(body, ub);

    /* 屋顶斜线 */
    int32_t roof_len = (int32_t)(w * 0.78);
    int32_t roof_x  = (s - roof_len) / 2;
    mk_line(icn, roof_x, y0, roof_len, MAX(s / 26, 2), 30, c);
    mk_line(icn, roof_x, y0, roof_len, MAX(s / 26, 2), -30, c);

    /* 门洞 */
    int32_t dw = (int32_t)(w * 0.28);
    int32_t dh = (int32_t)(h * 0.40);
    int32_t dx = (s - dw) / 2;
    int32_t dy = s - pad - dh;
    lv_obj_t * door = lv_obj_create(icn);
    lv_obj_set_pos(door, dx, dy);
    lv_obj_set_size(door, dw, dh);
    lv_obj_set_style_bg_opa(door, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(door, 0, 0);
    lv_obj_set_style_outline_width(door, MAX(s / 36, 2), 0);
    lv_obj_set_style_outline_color(door, c, 0);
    lv_obj_set_style_radius(door, dw / 2, 0);
    lv_obj_set_clickable(door, false);
    uid_t * ud = (uid_t *)lv_malloc(sizeof(uid_t));
    ud->magic = UID_MAGIC; ud->color = c;
    lv_obj_set_user_data(door, ud);
}

/* ICON_SETTINGS：齿轮 */
static void mk_settings(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t cx = s / 2;
    int32_t cy = s / 2;
    int32_t core = (int32_t)(s * 0.32);
    lv_obj_t * core_o = lv_obj_create(icn);
    lv_obj_set_pos(core_o, cx - core / 2, cy - core / 2);
    lv_obj_set_size(core_o, core, core);
    lv_obj_set_style_bg_opa(core_o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(core_o, 0, 0);
    lv_obj_set_style_outline_width(core_o, MAX(s / 30, 1), 0);
    lv_obj_set_style_outline_color(core_o, c, 0);
    lv_obj_set_style_radius(core_o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(core_o, false);
    uid_t * u0 = (uid_t *)lv_malloc(sizeof(uid_t));
    u0->magic = UID_MAGIC; u0->color = c;
    lv_obj_set_user_data(core_o, u0);

    int32_t spoke_l = (int32_t)(s * 0.16);
    int32_t spoke_w = MAX(s / 14, 2);
    int32_t r_outer = (int32_t)(s * 0.36);
    for (int i = 0; i < 8; i++) {
        int a = i * 45;
        lv_obj_t * sp = lv_obj_create(icn);
        lv_obj_set_size(sp, spoke_l, spoke_w);
        /* 顶端位于 (cx, cy - r_outer)，绕中心旋转 0/45/90...° */
        lv_obj_set_pos(sp, cx - spoke_l / 2, cy - r_outer);
        lv_obj_set_style_bg_color(sp, c, 0);
        lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_radius(sp, spoke_w / 2, 0);
        lv_obj_set_style_transform_angle(sp, a * 10, 0);
        lv_obj_set_style_transform_pivot_x(sp, spoke_l / 2, 0);
        lv_obj_set_style_transform_pivot_y(sp, spoke_w / 2, 0);
        lv_obj_set_clickable(sp, false);
        uid_t * u = (uid_t *)lv_malloc(sizeof(uid_t));
        u->magic = UID_MAGIC; u->color = c;
        lv_obj_set_user_data(sp, u);
    }
}

/* ICON_BACK：左箭头（用两条线段组成 < 形，顶点在左） */
static void mk_back(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t cx = s / 2;
    int32_t cy = s / 2;
    int32_t thick = MAX(s / 14, 2);
    int32_t arm_l = (int32_t)(s * 0.36);     /* 每条臂的长度 */
    /* 上臂：顶点(cx-arm_l, cy) → 折角(cx, cy-arm_l) */
    /* 用 lv_line：水平分量 = arm_l，垂直分量 = arm_l，对角线 √2·arm_l */
    int32_t diag = (int32_t)(((float)arm_l) * 1.42f);

    static lv_point_precise_t pts_a[2];
    pts_a[0].x = cx - arm_l;
    pts_a[0].y = cy;
    pts_a[1].x = cx;
    pts_a[1].y = cy - arm_l;

    lv_obj_t * la = lv_line_create(icn);
    lv_line_set_points(la, pts_a, 2);
    lv_obj_set_style_line_color(la, c, 0);
    lv_obj_set_style_line_width(la, thick, 0);
    lv_obj_set_style_line_rounded(la, true, 0);

    static lv_point_precise_t pts_b[2];
    pts_b[0].x = cx - arm_l;
    pts_b[0].y = cy;
    pts_b[1].x = cx;
    pts_b[1].y = cy + arm_l;

    lv_obj_t * lb = lv_line_create(icn);
    lv_line_set_points(lb, pts_b, 2);
    lv_obj_set_style_line_color(lb, c, 0);
    lv_obj_set_style_line_width(lb, thick, 0);
    lv_obj_set_style_line_rounded(lb, true, 0);

    (void)diag;
}

/* ICON_CLOSE：× 两条交叉线 */
static void mk_close(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t cx = s / 2;
    int32_t cy = s / 2;
    int32_t thick = MAX(s / 14, 2);
    int32_t half = (int32_t)(s * 0.28);

    static lv_point_precise_t pts_a[2];
    pts_a[0].x = cx - half;
    pts_a[0].y = cy - half;
    pts_a[1].x = cx + half;
    pts_a[1].y = cy + half;

    lv_obj_t * la = lv_line_create(icn);
    lv_line_set_points(la, pts_a, 2);
    lv_obj_set_style_line_color(la, c, 0);
    lv_obj_set_style_line_width(la, thick, 0);
    lv_obj_set_style_line_rounded(la, true, 0);

    static lv_point_precise_t pts_b[2];
    pts_b[0].x = cx - half;
    pts_b[0].y = cy + half;
    pts_b[1].x = cx + half;
    pts_b[1].y = cy - half;

    lv_obj_t * lb = lv_line_create(icn);
    lv_line_set_points(lb, pts_b, 2);
    lv_obj_set_style_line_color(lb, c, 0);
    lv_obj_set_style_line_width(lb, thick, 0);
    lv_obj_set_style_line_rounded(lb, true, 0);
}

/* ICON_KEY_LOCKED：小锁头（无外环） */
static void mk_key_locked(lv_obj_t * icn, int32_t s, lv_color_t c)
{
    int32_t body_w = (int32_t)(s * 0.55);
    int32_t body_h = (int32_t)(s * 0.45);
    int32_t body_x = (s - body_w) / 2;
    int32_t body_y = (int32_t)(s * 0.45);
    int32_t thick = MAX(s / 18, 2);

    lv_obj_t * body = lv_obj_create(icn);
    lv_obj_set_pos(body, body_x, body_y);
    lv_obj_set_size(body, body_w, body_h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_outline_width(body, thick, 0);
    lv_obj_set_style_outline_color(body, c, 0);
    lv_obj_set_style_radius(body, s / 14, 0);
    lv_obj_set_clickable(body, false);
    uid_t * ub = (uid_t *)lv_malloc(sizeof(uid_t));
    ub->magic = UID_MAGIC; ub->color = c;
    lv_obj_set_user_data(body, ub);

    int32_t post_w = thick;
    int32_t post_h = (int32_t)(s * 0.28);
    int32_t post_y = body_y - post_h + thick;
    mk_rect(icn, body_x + body_w / 2 - body_w / 3, post_y,
            post_w, post_h, post_w / 2, c, true);
    mk_rect(icn, body_x + body_w / 2 + body_w / 3 - post_w, post_y,
            post_w, post_h, post_w / 2, c, true);
    mk_rect(icn, body_x + body_w / 2 - body_w / 3, post_y - post_w / 2,
            body_w * 2 / 3, post_w, post_w / 2, c, true);

    int32_t kh_r = MAX(s / 22, 3);
    int32_t kh_cx = s / 2;
    int32_t kh_cy = body_y + body_h / 2 - s / 32;
    mk_dot(icn, kh_cx, kh_cy, kh_r, c);
    mk_rect(icn, kh_cx - kh_r / 2, kh_cy + kh_r / 2,
            kh_r, body_h / 4, kh_r / 2, c, true);
}

/* ----- 工厂入口 ----- */
lv_obj_t * ui_icon_create(lv_obj_t * parent,
                          ui_icon_kind_t kind,
                          int32_t size,
                          lv_color_t color)
{
    lv_obj_t * icn = lv_obj_create(parent);
    lv_obj_set_size(icn, size, size);
    lv_obj_set_style_bg_opa(icn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icn, 0, 0);
    lv_obj_set_style_outline_width(icn, 0, 0);
    lv_obj_set_style_pad_all(icn, 0, 0);
    lv_obj_set_clickable(icn, false);
    lv_obj_set_scrollable(icn, false);
    tag_root(icn);

    switch (kind) {
        case UI_ICON_LOCK:       mk_lock(icn, size, color); break;
        case UI_ICON_KEYPAD:     mk_keypad(icn, size, color); break;
        case UI_ICON_FACE_SCAN:  mk_face_scan(icn, size, color); break;
        case UI_ICON_USER:       mk_user(icn, size, color); break;
        case UI_ICON_LIST:       mk_list(icn, size, color); break;
        case UI_ICON_LIST_TAB:   mk_list_tab(icn, size, color); break;
        case UI_ICON_CALENDAR:   mk_calendar(icn, size, color); break;
        case UI_ICON_SIGNAL:     mk_signal(icn, size, color); break;
        case UI_ICON_HOME:       mk_home(icn, size, color); break;
        case UI_ICON_SETTINGS:   mk_settings(icn, size, color); break;
        case UI_ICON_BACK:       mk_back(icn, size, color); break;
        case UI_ICON_CLOSE:      mk_close(icn, size, color); break;
        case UI_ICON_KEY_LOCKED: mk_key_locked(icn, size, color); break;
        default: break;
    }
    return icn;
}

void ui_icon_set_color(lv_obj_t * icon, lv_color_t color)
{
    if (!icon) return;
    lv_obj_tree_walk(icon, recolor_tree, &color);
}

/* ================================================================
 *  图标字体通道（UI 重设计 ui4）
 *
 * 与 ui_icon_create()（几何拼装）并存：导航/卡片/列表行/按钮这类「标准线性图标」
 * 一律走图标字体 —— 字号成体系、颜色跟 token、渲染是字形位图（比逐对象拼装更省）。
 *
 * 性能与纪律：
 *  - 零每帧分配：label 在页面/卡片构造期一次性创建，运行期只改 text_color；
 *  - 颜色只走 theme_color(role)，四套主题自动跟随，不出现任何 hex；
 *  - 板子无 FreeType，图标字体是编译进位图（fonts/lv_font_icon_16/20/24/32.c）。
 * ================================================================ */

/* 码位自检表：与 icons.h 的 ui_glyph_t 一一对应，新增图标必须同时补这里 */
static const struct {
    ui_glyph_t g;
    const char * name;
} GLYPH_TABLE[] = {
    { UI_GLYPH_HOME,       "home"      }, { UI_GLYPH_LOCK,      "lock"      },
    { UI_GLYPH_LOCK_OPEN,  "lock_open" }, { UI_GLYPH_PERSON,    "person"    },
    { UI_GLYPH_GROUP,      "group"     }, { UI_GLYPH_LIST,      "list"      },
    { UI_GLYPH_FACE,       "face"      }, { UI_GLYPH_WIFI,      "wifi"      },
    { UI_GLYPH_SETTINGS,   "settings"  }, { UI_GLYPH_SYSTEM,    "system"    },
    { UI_GLYPH_FINGER,     "finger"    }, { UI_GLYPH_BELL,      "bell"      },
    { UI_GLYPH_KEY,        "key"       }, { UI_GLYPH_PLUS,      "plus"      },
    { UI_GLYPH_CLOSE,      "close"     }, { UI_GLYPH_BACK,      "back"      },
    { UI_GLYPH_CHEVRON_R,  "chevron_r" }, { UI_GLYPH_SHIELD,    "shield"    },
    { UI_GLYPH_CAMERA,     "camera"    }, { UI_GLYPH_BATTERY,   "battery"   },
    { UI_GLYPH_POWER,      "power"     }, { UI_GLYPH_OK,        "ok"        },
    { UI_GLYPH_WARN,       "warn"      }, { UI_GLYPH_DANGER,    "danger"    },
    { UI_GLYPH_TIME,       "time"      }, { UI_GLYPH_DATE,      "date"      },
    { UI_GLYPH_STORAGE,    "storage"   }, { UI_GLYPH_UNDO,      "undo"      },
    { UI_GLYPH_DELETE,     "delete"    }, { UI_GLYPH_EDIT,      "edit"      },
    { UI_GLYPH_EYE,        "eye"       }, { UI_GLYPH_BOLT,      "bolt"      },
    { UI_GLYPH_DONE,       "done"      }, { UI_GLYPH_SEARCH,    "search"    },
    { UI_GLYPH_LOGOUT,     "logout"    }, { UI_GLYPH_TOGGLE_ON, "toggle_on" },
    { UI_GLYPH_TOGGLE_OFF, "toggle_off"}, { UI_GLYPH_QR,        "qr"        },
    { UI_GLYPH_LAN,        "lan"       }, { UI_GLYPH_PIN,       "pin"       },
};

const lv_font_t * ui_icon_font(int32_t px)
{
    if (px <= 16) return &lv_font_icon_16;
    if (px <= 20) return &lv_font_icon_20;
    if (px <= 24) return &lv_font_icon_24;
    return &lv_font_icon_32;
}

/* 码位 → UTF-8（图标码位全在 U+0800..U+FFFF，固定 3 字节） */
static void glyph_utf8(uint32_t cp, char out[4])
{
    out[0] = (char)(0xE0u | ((cp >> 12) & 0x0Fu));
    out[1] = (char)(0x80u | ((cp >>  6) & 0x3Fu));
    out[2] = (char)(0x80u | ( cp        & 0x3Fu));
    out[3] = '\0';
}

lv_obj_t * icon_label_colored(lv_obj_t * parent, ui_glyph_t glyph,
                              int32_t size, theme_role_t role)
{
    lv_obj_t * lb = lv_label_create(parent);
    char s[4];
    glyph_utf8((uint32_t)glyph, s);
    lv_label_set_text(lb, s);
    lv_obj_set_style_text_font(lb, ui_icon_font(size), 0);
    lv_obj_set_style_text_color(lb, theme_color(role), 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    return lb;
}

lv_obj_t * icon_label(lv_obj_t * parent, ui_glyph_t glyph, int32_t size)
{
    return icon_label_colored(parent, glyph, size, TH_TEXT);
}

void icon_font_selfcheck(void)
{
    static const int32_t SIZES[4] = { 16, 20, 24, 32 };
    int miss = 0;
    int total = 0;
    for (int i = 0; i < 4; i++) {
        const lv_font_t * f = ui_icon_font(SIZES[i]);
        if (f == NULL) {
            printf("[icon] MISS font size=%d (font ptr is NULL)\n", (int)SIZES[i]);
            miss++;
            continue;
        }
        for (unsigned k = 0; k < sizeof(GLYPH_TABLE) / sizeof(GLYPH_TABLE[0]); k++) {
            lv_font_glyph_dsc_t d;
            total++;
            if (!lv_font_get_glyph_dsc(f, &d, (uint32_t)GLYPH_TABLE[k].g, 0)) {
                printf("[icon] MISS glyph %s cp=0x%04X size=%d\n",
                       GLYPH_TABLE[k].name, (unsigned)GLYPH_TABLE[k].g, (int)SIZES[i]);
                miss++;
            }
            else if (i == 3) {
                /* 只在 32px 档打印一次命中信息，避免日志刷屏 */
                printf("[icon] ok %-10s cp=0x%04X w=%d h=%d adv=%d\n",
                       GLYPH_TABLE[k].name, (unsigned)GLYPH_TABLE[k].g,
                       (int)d.box_w, (int)d.box_h, (int)d.adv_w);
            }
        }
    }
    printf("[icon] selfcheck done: sizes=16/20/24/32 checks=%d miss=%d\n", total, miss);
}


/* 运行时换码位（不重建 label）：事件行 / 状态图标刷新走这条路径，
 * 避免为了换图标反复 create/free（A7 单核：少一次对象创建少一次布局）。 */
void icon_label_set_glyph(lv_obj_t * lb, ui_glyph_t glyph)
{
    if (lb == NULL) return;
    char s[4];
    glyph_utf8((uint32_t)glyph, s);
    lv_label_set_text(lb, s);
}
