/**
 * @file page_users.c
 * 用户管理 — 页面骨架（D24 拆分后）：页面创建/导航/共享状态定义与事件订阅；
 * 列表渲染在 page_users_rows.c，弹窗族与业务操作在 page_users_dlg.c
 * （契约见 page_users_internal.h）。
 */
#include "page_users.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/icons.h"               /* icon_label_colored：图标一律走图标字体 + theme token */
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "core/event_bus.h"
#include "core/auth/unlock_backend.h"   /* backend_admin_verify_totp：改管理员密码动态码二次确认 */
#include "hal/hal_face.h"               /* face_service_enroll/delete_async + caps（UI 现代化 ui3） */
#include "hal/hal_time.h"               /* R2：时间源统一走 HAL，不直接读系统时钟 */
#include "ui/pages/page_enroll.h"       /* page_enroll_show_result：录入结果页内展示 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "page_users_internal.h"

/* 文件内前向声明（定义在 create 之后） */
static void go_back_cb(lv_event_t * e);

/* 列表容器 */
/* S1：『添加用户』按钮句柄 —— 数据读取失败时禁用它，避免用户在文件损坏的
 * 情况下点下去，把损坏的数据文件覆盖掉（不可逆）。 */
lv_obj_t * s_add_btn = NULL;
lv_obj_t * s_list = NULL;
/* 顶部统计行（FR-28 分区后承载「总人数 / 各角色人数」，空分区隐藏时信息不丢） */
lv_obj_t * s_summary = NULL;

/* 当前选中用户 ID（供弹窗回调使用） */
int s_sel_id = -1;


lv_obj_t * page_users_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    /* 页面根不自滚动：纵向滚动统一交给列表容器 s_list（见其 scroll_dir=VER），
     * 否则整页会被拖走、顶栏跟着跑（本页 scrollbar 家族问题的同一根因）。 */
    lv_obj_set_scrollable(root, false);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, SX(16), 0);
    lv_obj_set_style_pad_row(root, SY(8), 0);

    /* ------ 标题行 ------ */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* ★ LVGL v9 的 lv_obj 默认可滚动（lv_button 会在构造里自行关掉，lv_obj 不会）：
     *    所有纯装饰/布局容器一律显式关滚动，否则会出现可拖动并渲染滚动条
     *    （本页头像圆就是这个原因，见 users_list_loaded 的头像圆处注释）。 */
    lv_obj_set_scrollable(head, false);

    /* 按钮挂载到 head 后由父对象持有，无需保存句柄（原写法留下未使用变量 back，D11） */
    ui_icon_text_button(head, LV_SYMBOL_LEFT, "返回",
                        SX(78), SY(44), &st_ghost_btn,
                        theme_color(TH_TEXT), go_back_cb, NULL);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "用户管理");  /* 用户管理 */
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(24), 0);
    lv_obj_set_style_pad_left(title, SX(16), 0);

    lv_obj_t * spacer_h = lv_obj_create(head);
    lv_obj_set_flex_grow(spacer_h, 1);
    /* ★ D25：占位必须显式给交叉轴（高度）—— lv_obj 默认尺寸是 LV_DPI_DEF(130)，
     * flex_grow 只管主轴（ROW=宽度），高度落回 130 会把 SIZE_CONTENT 的 head 撑到 170。
     * PC 2x 缩放视觉减半掩盖了它，板端 1x 全部现形。CONTENT+pad0 = 0 高。 */
    lv_obj_set_height(spacer_h, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(spacer_h, 0, 0);
    lv_obj_set_style_bg_opa(spacer_h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_h, 0, 0);
    lv_obj_set_scrollable(spacer_h, false);

    lv_obj_t * add = s_add_btn = ui_icon_text_button(head, LV_SYMBOL_PLUS, "添加用户",
                                          SX(120), SY(44), &st_accent_btn,
                                          theme_color(TH_ACCENT_INK), add_btn_cb, NULL);
    lv_obj_add_style(add, &st_accent_btn_pr, LV_STATE_PRESSED);

    /* ------ 元信息行：安全规则（左） + 用户统计（右） ------
     * 左：说明「为什么那一行按钮是灰的」，避免用户以为界面卡死。
     * 不用 st_text_mut（历史坑：该样式的 text_color 有时不级联到 label）→ 显式设色。
     * 右：FR-28 分区后空分区隐藏，人数构成改由这里承载，信息不丢。 */
    lv_obj_t * meta = lv_obj_create(root);
    lv_obj_set_size(meta, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta, 0, 0);
    lv_obj_set_style_pad_all(meta, 0, 0);
    lv_obj_set_scrollable(meta, false);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * rule = lv_label_create(meta);
    lv_label_set_text(rule, "安全规则：系统必须保留至少 1 名「启用」状态的管理员");
    lv_obj_set_style_text_font(rule, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(rule, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_flex_grow(rule, 1);

    s_summary = lv_label_create(meta);
    lv_label_set_text(s_summary, "共 0 位用户");
    lv_obj_set_style_text_font(s_summary, app_font_scaled(12), 0);
    lv_obj_set_style_text_color(s_summary, theme_color(TH_TEXT_MUT), 0);

    /* ------ 表头 ------
     * FR-28 落地后「角色」列已由分组标题承载，行内再挂一个角色药丸就是重复信息
     * —— 故表头与行内都去掉角色列，腾出的横向空间留给操作按钮（图标 + 文案）。 */
    lv_obj_t * hdr = lv_obj_create(root);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_bottom(hdr, SY(6), 0);
    lv_obj_set_style_pad_row(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(hdr, SX(8), 0);
    /* 与行内左右内缩对齐：列表容器 s_list pad 6 + 行内 pad 10 = 16 */
    lv_obj_set_style_pad_left(hdr, SX(16), 0);
    lv_obj_set_style_pad_right(hdr, SX(16), 0);
    lv_obj_set_scrollable(hdr, false);

    /* 表头列 — 与行内**逐段等宽对齐**（行结构：头像28 + 8 + 用户名170 + 8 + 弹性
     * + 8 + 状态72 + 8 + 8 + 8 + 操作组368）。表头同构排布：用户名 238、弹性占位、
     * 状态 72、8、操作 368 —— 两行的固定宽度之和相等（718），弹性占位吸收的宽差
     * 一致，所以「状态」「操作」正好压在行内对应列上方。 */
    lv_obj_t * h_name = lv_label_create(hdr);
    lv_label_set_text(h_name, "用户名");  /* 用户名 */
    lv_obj_set_style_text_font(h_name, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_name, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_name, SX(238), LV_SIZE_CONTENT);

    lv_obj_t * h_sp = lv_obj_create(hdr);
    lv_obj_set_flex_grow(h_sp, 1);
    lv_obj_set_height(h_sp, LV_SIZE_CONTENT);   /* ★ D25：同 spacer_h */
    lv_obj_set_style_pad_all(h_sp, 0, 0);
    lv_obj_set_style_bg_opa(h_sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_sp, 0, 0);
    lv_obj_set_scrollable(h_sp, false);

    lv_obj_t * h_st = lv_label_create(hdr);
    lv_label_set_text(h_st, "状态");  /* 状态 */
    lv_obj_set_style_text_font(h_st, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_st, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_st, SX(72), LV_SIZE_CONTENT);

    lv_obj_t * h_sp2 = lv_obj_create(hdr);
    lv_obj_set_size(h_sp2, SX(8), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(h_sp2, 0, 0);   /* ★ D25：CONTENT 高含默认 pad=40，清零 */
    lv_obj_set_style_bg_opa(h_sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_sp2, 0, 0);
    lv_obj_set_scrollable(h_sp2, false);

    lv_obj_t * h_ops = lv_label_create(hdr);
    lv_label_set_text(h_ops, "操作");  /* 操作 */
    lv_obj_set_style_text_font(h_ops, app_font_scaled(13), 0);
    lv_obj_set_style_text_color(h_ops, theme_color(TH_TEXT_MUT), 0);
    lv_obj_set_size(h_ops, SX(368), LV_SIZE_CONTENT);

    /* ------ 用户列表（全宽紧凑行） ------ */
    s_list = lv_obj_create(root);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_add_style(s_list, &st_panel, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, SY(8), 0);
    lv_obj_set_style_pad_all(s_list, SX(6), 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    rebuild_list();

    /* 人脸录入/删除的异步应答（UI 现代化 ui3）：本页订阅 ENROLL_DONE / DELETE_DONE，
     * 负责把模组分配的模板号写回 users.json 并刷新列表；横幅由 ui_feedback 出。 */
    event_bus_subscribe(EV_FACE_EVENT, on_face_event_ui, NULL);
    /* 别处改动了用户数据 → 重新拉列表（见 on_user_changed 的注释）。 */
    event_bus_subscribe(EV_USER_CHANGED, on_user_changed, NULL);

    return root;
}

/* ------ 导航回调 ------ */
static void go_back_cb(lv_event_t * e)
{
    (void)e;
    /* 用户管理现为底部「用户」页签（一级页），返回主页即可 */
    ui_switch_page(PAGE_HOME);
}


