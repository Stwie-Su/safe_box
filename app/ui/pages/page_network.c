/**
 * @file page_network.c
 * 网络（DESIGN.md §7.5）：可用 WiFi（真实扫描）+ 已存网络（wpa 配置清单）。
 *
 * 数据来源（D10 WLAN 落地后全部真实，无 mock）：
 *   - 可用列表 = hal_net_scan()   —— wpa_supplicant ctrl_iface 扫描；
 *   - 已存列表 = hal_net_saved()  —— LIST_NETWORKS，即「真正会被用来连」的配置；
 *   - 连接动作 = hal_net_connect()—— SSID/PSK 以 hex 下发（免转义注入）；
 *   - 凭据保存 = store net_add_wifi()（可逆加密落盘，FR-6）。
 *
 * ★ 时序纪律：hal_net_* 全部阻塞（扫描可达数秒），只能在 worker_post 里跑；
 *   done 回调回到主线程后才允许碰 LVGL 对象。无无线网卡时 hal 自动降级
 *   none 后端，本页显示「无无线网卡」，不弹错误框（与 camera 降级同风格）。
 */
#include "page_network.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "core/store/store.h"
#include "core/support/async_store.h"
#include "core/support/worker.h"
#include "core/err.h"
#include "hal/hal_net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 一次扫描的容量上限。列表本身可滚动，24 个热点对室内场景足够；
 * 更多热点被截断是刻意取舍：无限列表要引入滚动定位与生命周期管理，不值得。 */
#define NET_MAX_AP    24
#define NET_MAX_SAVED 16

static lv_obj_t * s_avail_list;
static lv_obj_t * s_saved_list;
static lv_obj_t * s_status;

static lv_obj_t * s_ov = NULL;
static lv_obj_t * s_win = NULL;
static lv_obj_t * s_kb  = NULL;
static lv_obj_t * s_msg = NULL;
static char s_cur_ssid[64] = {0};

static void rebuild_all(void);
static void net_connect_async(const char * ssid, const char * psk);
static void go_back_cb(lv_event_t * e);
static void refresh_cb(lv_event_t * e);
static void wifi_click_cb(lv_event_t * e);
static void net_conn_done(int result, const char * psk);
static void net_add_done(int result);
static void dlg_psk(void);
static void close_dlg(void);
static void kb_focus_cb(lv_event_t * e);
static void psk_save_cb(lv_event_t * e);
static void dlg_cancel_cb(lv_event_t * e);

lv_obj_t * page_network_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, SX(20), 0);
    lv_obj_set_style_pad_row(root, SY(12), 0);

    /* 标题行 */
    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * back = lv_button_create(head);
    lv_obj_set_size(back, SX(72), SY(44));
    lv_obj_add_style(back, &st_ghost_btn, 0);
    lv_obj_add_event_cb(back, go_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, "‹ 返回");
    lv_obj_set_style_text_font(bl, app_font_scaled(14), 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(head);
    lv_label_set_text(title, "网络");
    lv_obj_add_style(title, &st_text, 0);
    lv_obj_set_style_text_font(title, app_font_scaled(28), 0);
    lv_obj_set_style_pad_left(title, 16, 0);

    s_status = lv_label_create(head);
    lv_label_set_text(s_status, "…");  /* 真实状态由后台查询回填；占位避免默认 "Text" */
    lv_obj_add_style(s_status, &st_ok_text, 0);
    lv_obj_set_style_text_font(s_status, app_font_scaled(14), 0);
    lv_obj_set_style_pad_left(s_status, 20, 0);

    /* 弹性占位 + 刷新按钮（FR：网络页手动刷新）——扫描/已存/状态三者都由
     * rebuild_all() 的一个 worker 作业重取；点击后列表先回到占位态给即时反馈。 */
    lv_obj_t * sp = lv_obj_create(head);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_set_scrollable(sp, false);

    ui_icon_text_button(head, LV_SYMBOL_REFRESH, "刷新",
                        SX(90), SY(44), &st_ghost_btn,
                        theme_color(TH_TEXT), refresh_cb, NULL);

    /* 主体：左可用 + 右已存 */
    lv_obj_t * body = lv_obj_create(root);
    /* 宽度 = 父容器宽度，flex_grow 负责在 ROW 主轴上吃掉剩余空间
     * （与 page_system.c 的 left/right 同一写法）。 */
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, SX(16), 0);

    /* 左：可用网络 */
    lv_obj_t * avail = lv_obj_create(body);
    lv_obj_set_flex_grow(avail, 1);
    /* ★ 关键：body 是 ROW flex，flex_grow 只管**主轴（宽度）**。
     *   高度属交叉轴，必须显式给 —— 否则按内容高排，卡片塌成约 130px，
     *   下方留一大片空白。对照 page_system.c 的 left/right 写法。 */
    lv_obj_set_height(avail, lv_pct(100));
    lv_obj_add_style(avail, &st_panel, 0);
    lv_obj_set_flex_flow(avail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(avail, 8, 0);

    lv_obj_t * at = lv_label_create(avail);
    lv_label_set_text(at, "可用网络（扫描）");
    lv_obj_add_style(at, &st_text, 0);
    lv_obj_set_style_text_font(at, app_font_scaled(20), 0);

    s_avail_list = lv_obj_create(avail);
    /* 父容器是 COLUMN flex，这里 flex_grow 管的是**高度**（主轴） */
    lv_obj_set_width(s_avail_list, lv_pct(100));
    lv_obj_set_flex_grow(s_avail_list, 1);
    lv_obj_set_style_bg_opa(s_avail_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_avail_list, 0, 0);
    lv_obj_set_flex_flow(s_avail_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_avail_list, 6, 0);

    /* 列表内容由后台扫描回填：先放占位。扫描是秒级阻塞操作，
     * 必须在 worker 线程跑（见 hal_net.h 的时序约定），这里绝不能同步调。 */
    lv_obj_t * tip = lv_label_create(s_avail_list);
    lv_label_set_text(tip, "扫描中…");
    lv_obj_add_style(tip, &st_text_mut, 0);
    lv_obj_set_style_text_font(tip, app_font_scaled(14), 0);

    /* 右：已存网络 */
    lv_obj_t * saved = lv_obj_create(body);
    lv_obj_set_flex_grow(saved, 1);
    lv_obj_set_height(saved, lv_pct(100));   /* ★ 同上：交叉轴高度必须显式给 */
    lv_obj_add_style(saved, &st_panel, 0);
    lv_obj_set_flex_flow(saved, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(saved, 8, 0);

    lv_obj_t * st = lv_label_create(saved);
    lv_label_set_text(st, "已存网络");
    lv_obj_add_style(st, &st_text, 0);
    lv_obj_set_style_text_font(st, app_font_scaled(20), 0);

    s_saved_list = lv_obj_create(saved);
    /* 父容器是 COLUMN flex，这里 flex_grow 管的是**高度**（主轴） */
    lv_obj_set_width(s_saved_list, lv_pct(100));
    lv_obj_set_flex_grow(s_saved_list, 1);
    lv_obj_set_style_bg_opa(s_saved_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_saved_list, 6, 0);

    rebuild_all();
    return root;
}

static void go_back_cb(lv_event_t * e)
{
    (void)e;
    ui_switch_page(PAGE_HOME);   /* FR-7 变更：设置中枢已改为开发者选项，返回目标改主页 */
}

/* 刷新：列表清回占位态给即时反馈，数据由 rebuild_all() 的 worker 作业回填
 *（扫描可达数秒，时序纪律同本页其它 hal_net_* 调用）。 */
static void refresh_cb(lv_event_t * e)
{
    (void)e;
    lv_label_set_text(s_status, "刷新中…");

    lv_obj_clean(s_avail_list);
    lv_obj_t * tip = lv_label_create(s_avail_list);
    lv_label_set_text(tip, "扫描中…");
    lv_obj_add_style(tip, &st_text_mut, 0);
    lv_obj_set_style_text_font(tip, app_font_scaled(14), 0);

    lv_obj_clean(s_saved_list);
    lv_obj_t * tip2 = lv_label_create(s_saved_list);
    lv_label_set_text(tip2, "读取中…");
    lv_obj_add_style(tip2, &st_text_mut, 0);
    lv_obj_set_style_text_font(tip2, app_font_scaled(14), 0);

    rebuild_all();
}

/* ---------------- 后台取数：扫描 + 已存 + 状态，一个作业全拿 ----------------
 * 三者都要走 ctrl_iface 往返，拆成三个 job 会让扫描的几秒里另外两块一直空着；
 * 合成一个 job 一次回填，UI 只闪一次。 */
typedef struct {
    int          avail;                    /* hal_net_available() */
    int          nap_n;                    /* 扫描到的热点数（<0 视为 0） */
    wpa_ap_t     ap[NET_MAX_AP];
    int          nsaved;
    net_saved_t  saved[NET_MAX_SAVED];
    net_status_t st;
} net_data_job_t;

/* 扫描结果要活过 job 的生命周期（点击回调要用），放静态区。
 * 只在主线程读写（worker 填 job、done 拷进来），无竞争。 */
static wpa_ap_t s_scan_ap[NET_MAX_AP];
static int      s_scan_n = 0;

static const char * signal_bars(int dbm)
{
    /* 与原 mock 相同的阶梯字符，字体已验证可显示；
     * 分级口径：-55 以上满格 / -67 良 / -75 中 / 更弱一格。 */
    if (dbm >= -55) return "▂▄▆█";
    if (dbm >= -67) return "▂▄▆";
    if (dbm >= -75) return "▂▄";
    return "▂";
}

static const char * state_text(net_state_t s)
{
    switch (s) {
    case NET_STATE_CONNECTED:   return "已连接";
    case NET_STATE_CONNECTING:  return "连接中…";
    case NET_STATE_SCANNING:    return "扫描中…";
    case NET_STATE_DOWN:        return "未连接";
    default:                    return "状态未知";
    }
}

static void net_data_worker(void * p)
{
    net_data_job_t * a = (net_data_job_t *)p;

    a->avail  = hal_net_available();
    a->nap_n  = 0;
    a->nsaved = 0;
    memset(&a->st, 0, sizeof(a->st));

    if (!a->avail) return;   /* 连网卡都没有，剩余查询必然失败，别浪费时间 */

    int n = hal_net_scan(a->ap, NET_MAX_AP);
    a->nap_n = (n > 0) ? n : 0;              /* 负值按 0 处理，UI 显示「未扫到」 */

    n = hal_net_saved(a->saved, NET_MAX_SAVED);
    a->nsaved = (n > 0) ? n : 0;

    if (hal_net_status(&a->st) != SAFE_OK) memset(&a->st, 0, sizeof(a->st));
}

static void avail_add_row(const wpa_ap_t * ap, int idx)
{
    lv_obj_t * item = lv_button_create(s_avail_list);
    lv_obj_set_size(item, lv_pct(100), SY(48));
    lv_obj_add_style(item, &st_panel2, 0);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_add_event_cb(item, wifi_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    lv_obj_t * nm = lv_label_create(item);
    lv_label_set_text(nm, ap->ssid[0] ? ap->ssid : "(隐藏网络)");
    lv_obj_add_style(nm, &st_text, 0);
    lv_obj_set_style_text_font(nm, app_font_scaled(16), 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t * sig = lv_label_create(item);
    lv_label_set_text(sig, signal_bars(ap->signal_dbm));
    lv_obj_add_style(sig, &st_ok_text, 0);
    lv_obj_set_style_text_font(sig, app_font_scaled(14), 0);
    lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void net_data_done(void * p)
{
    net_data_job_t * a = (net_data_job_t *)p;

    /* 1) 状态标签：以后台查到的真实状态为准 */
    lv_label_set_text(s_status, a->avail ? state_text(a->st.state) : "无无线网卡");
    lv_obj_add_style(s_status, (a->avail && a->st.state == NET_STATE_CONNECTED)
                     ? &st_ok_text : &st_text_mut, 0);

    /* 2) 可用网络 */
    s_scan_n = a->nap_n;
    memcpy(s_scan_ap, a->ap, sizeof(a->ap));
    lv_obj_clean(s_avail_list);
    if (!a->avail) {
        lv_obj_t * tip = lv_label_create(s_avail_list);
        lv_label_set_text(tip, "本机没有可管理的无线网卡");
        lv_obj_add_style(tip, &st_text_mut, 0);
        lv_obj_set_style_text_font(tip, app_font_scaled(14), 0);
    } else if (a->nap_n == 0) {
        lv_obj_t * tip = lv_label_create(s_avail_list);
        lv_label_set_text(tip, "未扫描到网络");
        lv_obj_add_style(tip, &st_text_mut, 0);
        lv_obj_set_style_text_font(tip, app_font_scaled(14), 0);
    } else {
        for (int i = 0; i < a->nap_n; i++) avail_add_row(&a->ap[i], i);
    }

    /* 3) 已存网络（wpa 里真正生效的配置，不是凭据备份库） */
    lv_obj_clean(s_saved_list);
    for (int i = 0; i < a->nsaved; i++) {
        lv_obj_t * item = lv_obj_create(s_saved_list);
        lv_obj_set_size(item, lv_pct(100), 44);
        lv_obj_add_style(item, &st_panel2, 0);
        lv_obj_set_style_radius(item, 8, 0);

        lv_obj_t * nm = lv_label_create(item);
        lv_label_set_text(nm, a->saved[i].ssid[0] ? a->saved[i].ssid : "(未命名配置)");
        lv_obj_add_style(nm, &st_text, 0);
        lv_obj_set_style_text_font(nm, app_font_scaled(14), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t * cur = (a->st.state == NET_STATE_CONNECTED &&
                          strcmp(a->st.ssid, a->saved[i].ssid) == 0)
                         ? lv_label_create(item) : NULL;
        if (cur) {
            lv_label_set_text(cur, "使用中");
            lv_obj_add_style(cur, &st_ok_text, 0);
            lv_obj_set_style_text_font(cur, app_font_scaled(14), 0);
            lv_obj_align(cur, LV_ALIGN_RIGHT_MID, -14, 0);
        }
    }
    if (a->nsaved == 0) {
        lv_obj_t * tip = lv_label_create(s_saved_list);
        lv_label_set_text(tip, "暂无已存网络");
        lv_obj_add_style(tip, &st_text_mut, 0);
        lv_obj_set_style_text_font(tip, app_font_scaled(14), 0);
    }

    free(a);
}

static void rebuild_all(void)
{
    net_data_job_t * a = (net_data_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    worker_post(net_data_worker, a, net_data_done);
}

/* ---------------- 连接下发（同样必须异步） ---------------- */
typedef struct {
    int  rc;
    char ssid[64];
    char psk[128];
} net_conn_job_t;

static void net_conn_worker(void * p)
{
    net_conn_job_t * a = (net_conn_job_t *)p;
    a->rc = hal_net_connect(a->ssid, a->psk);
}

static void net_conn_ui_done(void * p)
{
    net_conn_job_t * a = (net_conn_job_t *)p;
    if (a->rc == SAFE_OK) {
        astore_append_log("wifi_change", "admin", 1, a->ssid);
        /* 不能在这里谎称「已连接」：connect 返回只代表配置已下发，
         * 握手与 DHCP 都是异步的。让用户看到「连接中…」，随后刷新取真实状态。 */
        lv_label_set_text(s_status, "连接中…");
        rebuild_all();
    } else {
        lv_label_set_text(s_status, "连接失败");
    }
    free(a);
}

static void net_connect_async(const char * ssid, const char * psk)
{
    net_conn_job_t * a = (net_conn_job_t *)calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->ssid, ssid ? ssid : "", sizeof(a->ssid) - 1);
    strncpy(a->psk, psk ? psk : "", sizeof(a->psk) - 1);
    worker_post(net_conn_worker, a, net_conn_ui_done);
}

/* 已存网络直连：后台查 psk（AES+PBKDF2），回调里决定直连或弹 PSK 输入 */
static void net_conn_done(int result, const char * psk)
{
    if (result != 0) {
        dlg_psk();   /* 凭据库里没有：让用户输入 */
        return;
    }
    net_connect_async(s_cur_ssid, psk);   /* 有凭据：直接下发（psk 空串=开放网络） */
}

static void wifi_click_cb(lv_event_t * e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    /* 扫描数据是异步回填的，极端时序下（连点）可能越界，必须防御 */
    if (idx < 0 || idx >= s_scan_n) return;
    strncpy(s_cur_ssid, s_scan_ap[idx].ssid, sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';

    astore_net_get_psk(s_cur_ssid, net_conn_done);
}

/* ---------------- PSK 输入弹窗 ---------------- */
static void dlg_psk(void)
{
    close_dlg();
    lv_obj_t * scr = lv_screen_active();
    s_ov = lv_obj_create(scr);
    lv_obj_set_size(s_ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);

    /* ★ 尺寸用常量算：按钮坐标要拿它做绝对定位，而**不能**在 set_size 之后
     * 回查 lv_obj_get_height(s_win) —— 刚建的对象要到下一次布局才有 coords，
     * 此处查询恒为 0，按钮会落到负坐标画不出来（page_users.c 的
     * dlg_bottom_btn_xy 上方早有同一陷阱的注释）。 */
    const int32_t win_w = 380, win_h = 200;
    s_win = lv_obj_create(s_ov);
    lv_obj_set_size(s_win, win_w, win_h);
    lv_obj_add_style(s_win, &st_panel, 0);
    lv_obj_set_style_radius(s_win, 16, 0);
    lv_obj_align(s_win, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(s_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_win, 8, 0);

    char tbuf[80];
    snprintf(tbuf, sizeof(tbuf), "连接 %s", s_cur_ssid);
    lv_obj_t * t = lv_label_create(s_win);
    lv_label_set_text(t, tbuf);
    lv_obj_add_style(t, &st_text, 0);
    lv_obj_set_style_text_font(t, app_font_scaled(20), 0);
    /* 限宽折行：SSID 可能很长，不限宽会被 LVGL 排成单行冲出窗口右边界 */
    lv_obj_set_width(t, win_w - 32);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    s_msg = lv_label_create(s_win);
    lv_label_set_text(s_msg, " ");
    lv_obj_add_style(s_msg, &st_warn_text, 0);
    lv_obj_set_style_text_font(s_msg, app_font_scaled(14), 0);

    lv_obj_t * ta = lv_textarea_create(s_win);
    lv_obj_set_size(ta, SX(300), SY(40));
    lv_obj_add_style(ta, &st_panel2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, 32);
    lv_textarea_set_placeholder_text(ta, "WiFi 密码");
    lv_obj_set_style_text_color(ta, theme_color(TH_TEXT_PLACEHOLDER), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(ta, app_font_scaled(14), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(ta, theme_color(TH_TEXT), 0);
    lv_obj_add_event_cb(ta, kb_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* 底部操作按钮：FLOATING 脱离 flex 布局 + 绝对定位（见 page_users 同样注释） */
    const int32_t btn_y = win_h - 16 - 44 - 16;
    const int32_t btn_x = (win_w - 32 - 300) / 2 + 16;

    lv_obj_t * cc = lv_button_create(s_win);
    lv_obj_set_size(cc, 146, 44);
    lv_obj_set_pos(cc, btn_x, btn_y);
    lv_obj_set_floating(cc, true);
    lv_obj_add_style(cc, &st_ghost_btn, 0);
    lv_obj_add_event_cb(cc, dlg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "取消");
    lv_obj_set_style_text_font(ccl, app_font_scaled(14), 0);
    lv_obj_center(ccl);

    lv_obj_t * ok = lv_button_create(s_win);
    lv_obj_set_size(ok, 146, 44);
    lv_obj_set_pos(ok, btn_x + 154, btn_y);
    lv_obj_set_floating(ok, true);
    lv_obj_add_style(ok, &st_accent_btn, 0);
    lv_obj_add_style(ok, &st_accent_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, psk_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok);
    lv_label_set_text(okl, "连接");
    lv_obj_set_style_text_font(okl, app_font_scaled(14), 0);
    lv_obj_center(okl);

    s_kb = lv_keyboard_create(s_ov);
    lv_obj_set_size(s_kb, lv_pct(100), SY(210));   /* ★ 随窗口比例缩放 */
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, theme_color(TH_PANEL), 0);
    /* ★ 按键文本属于 LV_PART_ITEMS；且必须走 Montserrat —— 键盘上的 ⌫ ↩ ⏎ 是
     * LVGL 符号码位，中文字体里没有，套上去会渲染成方块。 */
    lv_obj_set_style_text_font(s_kb, app_montserrat_scaled(32), LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_kb, ta);
}

static void close_dlg(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    s_ov = NULL; s_win = NULL; s_kb = NULL; s_msg = NULL;
}

static void kb_focus_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target(e);
    if (s_kb) lv_keyboard_set_textarea(s_kb, ta);
}

static void dlg_cancel_cb(lv_event_t * e)
{
    (void)e;
    close_dlg();
}

static void psk_save_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * ta = NULL;
    uint32_t cnt = lv_obj_get_child_count(s_win);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t * c = lv_obj_get_child(s_win, i);
        if (lv_obj_check_type(c, &lv_textarea_class)) { ta = c; break; }
    }
    if (!ta) return;
    const char * psk = lv_textarea_get_text(ta);
    if (strlen(psk) < 8) {
        lv_label_set_text(s_msg, "密码至少 8 位");
        return;
    }
    astore_net_add_wifi(s_cur_ssid, "WPA2", psk, net_add_done);
}

static void net_add_done(int result)
{
    if (result != 0) {
        lv_label_set_text(s_msg, "保存失败");
        return;
    }
    close_dlg();
    /* 凭据已落盘（FR-6）。astore 回调不回传明文 PSK（设计如此，减少明文扩散），
     * 这里再异步取一次再下发连接；取不到会走 dlg_psk 让用户重输。 */
    astore_net_get_psk(s_cur_ssid, net_conn_done);
}
