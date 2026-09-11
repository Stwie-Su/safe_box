/**
 * @file page_face.h
 * 人脸识别全屏页：从主页"人脸识别"卡片进入，展示扫描动效 + 实时视频预览。
 *
 * UI 现代化（spec §3 后半）新增「扫描框状态染色」：
 * 四角扫描框 + 扫描光带按识别结果染色（IDLE=accent / OK=绿 / FAIL=红 / WARN=黄），
 * 由反馈中枢 ui_feedback 在收到 EV_FACE_EVENT 时调用 page_face_set_scan_state() 驱动。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 扫描框状态（spec §3：枚举 + 查表，避免 if-else 嵌套） */
typedef enum {
    PAGE_FACE_SCAN_IDLE = 0,   /* 待机：accent（扫描中） */
    PAGE_FACE_SCAN_OK,         /* 匹配成功：TH_OK 绿 */
    PAGE_FACE_SCAN_FAIL,       /* 未匹配 / 活体失败：TH_DANGER 红（600ms 后自动回 IDLE） */
    PAGE_FACE_SCAN_WARN,       /* 警示：TH_WARN 黄 */
    PAGE_FACE_SCAN_COUNT
} page_face_scan_t;

/**
 * @brief 设置扫描框（8 个角 + 光带）状态色。
 *
 * 性能预算（A7 单核）：只改 9 个对象的 bg_color 一个样式属性，无动画对象、
 * 无堆分配；FAIL 态用一次性 lv_timer（UI_ANIM_SCAN_FAIL_MS）自动回 IDLE。
 */
void page_face_set_scan_state(page_face_scan_t st);

/**
 * @brief 读取当前扫描框状态（主题切换重染色与自测用）。
 */
page_face_scan_t page_face_scan_state(void);

lv_obj_t * page_face_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif
