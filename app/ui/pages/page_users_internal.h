/**
 * @file page_users_internal.h
 * 用户管理页拆分契约（D24）：page_users.c（页面骨架）+ page_users_rows.c
 * （列表/行渲染）+ page_users_dlg.c（弹窗族与业务操作）。
 * 仅三文件之间共享的符号放这里；页面对外接口仍在 page_users.h。
 * 拆分原则：纯搬移零逻辑改动；共享状态集中定义在骨架文件（page_users.c）。
 */
#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include "core/event_bus.h"

/* ---- 共享状态（定义在 page_users.c）---- */
extern lv_obj_t * s_add_btn;    /* 「添加用户」按钮：数据读取失败时禁用 */
extern lv_obj_t * s_list;      /* 用户列表容器 */
extern lv_obj_t * s_summary;  /* 顶部统计行 */
extern int        s_sel_id;   /* 当前行选中用户 id（rows 回调设置，弹窗读取） */

/* ---- rows → 其他 ---- */
const char * name_cache_get(int id);   /* 弹窗预填用户名（主线程读内存） */
bool         face_cap_ok(bool need_enroll);
void         rebuild_list(void);       /* 弹窗操作完成后刷列表 */
void         on_user_changed(ev_topic_t topic, const void * payload, void * user);

/* ---- dlg → 其他（「添加用户」按钮与行内回调的入口；事件订阅）---- */
void add_btn_cb(lv_event_t * e);
void pwd_btn_cb(lv_event_t * e);
void toggle_btn_cb(lv_event_t * e);
void del_btn_cb(lv_event_t * e);
void face_btn_cb(lv_event_t * e);
void on_face_event_ui(ev_topic_t topic, const void * payload, void * user);
