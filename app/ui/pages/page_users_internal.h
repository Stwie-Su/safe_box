/**
 * @file page_users_internal.h
 * 用户管理页内部拆成三个文件后，它们之间共享的东西放这里。
 *
 *   page_users.c      —— 页面骨架、共享状态、事件订阅
 *   page_users_rows.c —— 列表和每一行的渲染
 *   page_users_dlg.c  —— 弹窗和具体操作（增删改、改密码、录人脸）
 *
 * 对外接口还在 page_users.h，别的模块不要包含本文件。
 */
#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include "core/event_bus.h"

/* ---- 共享状态（定义在 page_users.c）---- */
extern lv_obj_t * s_add_btn;    /* 「添加用户」按钮：读取失败时置灰 */
extern lv_obj_t * s_list;      /* 用户列表容器 */
extern lv_obj_t * s_summary;  /* 顶部统计行 */
extern int        s_sel_id;   /* 当前选中的用户 id（行回调设置，弹窗读取） */

/* ---- 列表模块提供的接口 ---- */
const char * name_cache_get(int id);   /* 弹窗预填用户名（读内存缓存，很快） */
bool         face_cap_ok(bool need_enroll);  /* 模组还能不能录入 / 有没有空位 */
void         rebuild_list(void);       /* 数据变了之后刷新列表 */
void         on_user_changed(ev_topic_t topic, const void * payload, void * user);

/* ---- 弹窗模块里的回调（按钮和列表行会调）---- */
void add_btn_cb(lv_event_t * e);
void pwd_btn_cb(lv_event_t * e);
void toggle_btn_cb(lv_event_t * e);
void del_btn_cb(lv_event_t * e);
void face_btn_cb(lv_event_t * e);
void on_face_event_ui(ev_topic_t topic, const void * payload, void * user);
