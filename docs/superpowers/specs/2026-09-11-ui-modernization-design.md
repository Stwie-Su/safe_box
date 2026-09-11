# UI 现代化与功能接线设计（人脸反馈 / 页面过渡 / 按压手感 / 录入入口）

> 日期：2026-09-11
> 状态：已获用户批准（全案 + 克制现代风 + 扫描框状态/顶部横幅）
> 范围：`app/ui/**` + 少量 core 只读查询接口；不改 store / 不改后端
> 硬件约束：单核 Cortex-A7 @1024×600，动效全部小面积、≤250ms、无 blur / 无大阴影

## 0. 背景与问题

Sprint3 3a/3b/3c 已把人脸链路全通（`EV_FACE_EVENT` 总线广播 reason 五档、
`FACE_EV_ENROLL_DONE`/`DELETE_DONE`、FSM 按 FR-19 分流），但 UI 没接：

| 功能 | 链路 | UI 现状 |
|---|---|---|
| 识别结果（OK/NO_MATCH/LIVENESS_FAIL/…） | 总线广播 | 人脸页只有视频+光带，**零反馈** |
| WAIT_OTP / LOCKOUT | FSM → `fsm_ui_hook` | 硬切页；LOCKOUT 无倒计时提示 |
| 人脸录入/删除 | `face_service_enroll/delete_tpl` + 总线应答 | **UI 无入口**（只能 RPC 注入） |
| 连败计数（n/3） | `auth_fsm.c` 内部 `fail_streak` | 不可见 |
| 页面切换 | `ui.c switch_page` → `lv_obj_set_hidden` | 瞬间硬切 |
| 按压反馈 | theme.c 单一过渡雏形 | 颜色直跳 |

## 1. 架构：集中反馈模块 + 常驻 overlay（已选方案 A）

```
ui.c 布局（纵向四层，全部常驻同一 screen）：
  topbar  56px（不动）
  overlay 横幅容器（常驻、默认空、lv_obj_set_click_feedthrough 不挡触摸）
  content 页面区（10 页常驻，switch_page 动画在这里）
  tabbar  72px（不动）
```

新建 `app/ui/ui_feedback.[ch]`（反馈中枢）：

- `ui_feedback_init()`（`ui_init` 末尾调用）：订阅 `EV_FACE_EVENT` + `EV_AUTH_RESULT`
  （多订阅者合法：app.c 已订 `EV_AUTH_RESULT`，auth_fsm 已订 `EV_FACE_EVENT`；
  横幅跨页存活是硬需求——LOCKOUT 时 FSM 强制切回主页，各页面自订会丢事件）
- `ui_banner(const char *text, ui_banner_level_t level, uint32_t hold_ms)`：
  三档主题色（BANNER_INFO=accent / BANNER_OK=TH_OK / BANNER_WARN=TH_WARN /
  BANNER_DANGER=TH_DANGER）；lv_anim 淡入 250ms → 停留 hold_ms → 淡出 250ms → hidden；
  新横幅到达时删旧动画直接替换
- 事件 → 横幅映射：

| 事件 | 横幅 | 扫描框 |
|---|---|---|
| `FACE_EV_DETECT` reason=OK | 「欢迎回来，{用户名}」（OK，2s） | 绿 |
| NO_MATCH | 「未匹配（n/face_otp_after）」（WARN，1.5s） | 红 600ms 回 accent |
| LIVENESS_FAIL | 「活体检测失败」（DANGER，2s） | 红 |
| TIMEOUT/ERROR | 「识别超时/识别异常」（INFO，1.5s） | 不变 |
| `FACE_EV_ENROLL_DONE` err=OK | 「人脸录入成功」（OK，2s） | — |
| `FACE_EV_ENROLL_DONE` err≠OK | 「人脸录入失败」（DANGER，2s） | — |
| `FACE_EV_DELETE_DONE` | 成功/失败横幅（OK/DANGER） | — |
| `EV_AUTH_RESULT` DENY 触发 LOCKOUT | 「多次失败，已锁定 {n}s 后重试」每秒刷新（DANGER） | — |

- 用户名反查：`FACE_EV_DETECT` payload `res.face_id` → `user_find_by_face()`
  （已有接口，store.h:74）
- LOCKOUT 倒计时：`auth_fsm_lock_remaining()`（已有，auth_fsm.h:53）+ `lv_timer`
  每秒刷新横幅文字；锁定解除（remaining=0）自动淡出
- 唯一 core 接口缺口：`auth_fsm_fail_streak()`——`fail_streak` 是 auth_fsm.c
  内部静态（auth_fsm.c:42），未暴露。**新增只读查询接口**（接口变更先改
  《技术路线规约》§5.2，再动代码）

## 2. 页面切换过渡（`ui.c switch_page`）

- 现 `lv_obj_set_hidden(s_page_roots[i], …)`（ui.c:496）硬切 → 双段动画：
  - 旧页：`translate_y(0→-24px)` + `opa(255→0)`，150ms ease-in
  - 新页：`translate_y(24px→0)` + `opa(0→255)`，200ms ease-out
- 页面 roots 常驻（ui.c:139 一次性实例化），动画只动样式属性、不销毁对象
- 快速连点：`lv_anim_delete(obj, NULL)` 清旧动画再起（同一对象同 prop 单动画是
  LVGL 约束）
- FSM 自动切页（`fsm_ui_hook` WAIT_OTP→PAGE_OTP）走同一路径自动获得动画
- 动画常量集中 `ui_anim.h` 一处（时长/位移/缓动，便于 A7 板上调参）

## 3. 按压手感 + 扫描框状态

### theme.c（按压过渡升级）
- 过渡属性集扩为 `{BG_COLOR, SHADOW_WIDTH, SHADOW_OFFSET_Y, TRANSFORM_WIDTH,
  TRANSFORM_HEIGHT}`；进按压 120ms / 回弹 180ms
- `st_accent_btn` 按压态：`transform_width(-6)/transform_height(-3)`（≈2% 微缩）
  + 阴影收紧——全工程强调按钮统一获得反馈；ghost/danger/tab 按钮按同模式跟进
- tab 按钮 checked 过渡（页签高亮切换 150ms 颜色渐变）

### page_face.c（扫描框状态染色）
- `page_face.h` 新增：
  ```c
  typedef enum { PAGE_FACE_SCAN_IDLE, PAGE_FACE_SCAN_OK,
                 PAGE_FACE_SCAN_FAIL, PAGE_FACE_SCAN_WARN } page_face_scan_t;
  void page_face_set_scan_state(page_face_scan_t st);
  ```
- 实现：8 个 corner（s_corners[8]）+ 光带颜色统一刷新；FAIL 态 600ms 后自动回
  IDLE（lv_timer 一次性）；主题切换时按当前状态重染色（theme_change_cb 链）

## 4. 人脸录入/删除入口（page_users.c）

- 用户条目行加「人脸」操作：
  - `face_id<0` → 「录入人脸」→ 管理员确认（复用现有 admin 确认弹窗模式，
    与改密同级敏感操作）→ `face_service_enroll()`
  - `face_id≥0` → 「删除人脸」→ 同上确认 → `face_service_delete_tpl(u->face_id)`
- 应答（`EV_FACE_EVENT` ENROLL_DONE/DELETE_DONE）→ ui_feedback 横幅；
  录入成功时把模组分配的 uid 写回该用户的 `face_id`（`safe_user_t.face_id`
  已存在，store.h:43；store.c:475 已兼容老数据缺字段）。
  **store 接口缺口**：现无单字段写回接口（`user_update` 是整记录覆盖，
  「查→改→写回」三步有丢失并发更新风险）——新增便捷接口
  `user_face_set(int user_id, int face_id)`（-1=清除），内部读改写整记录并
  落盘；**接口变更先改《技术路线规约》§5.6 再动代码**。
  删除路径同用该接口写 -1，并调 `face_service_delete_tpl(u->face_id)`
- 页面刷新：应答到达后重载该用户条目（现有列表刷新路径）
- 后端不支持时（`SAFE_FACE_BACKEND=none`）按钮置灰（`face_service_caps()`
  查能力，hal_face.h 已有 caps 机制）

## 5. 分层与纪律

- `ui_feedback` 在 ui 层：订阅总线属事件绑定（app.c 已有订阅先例），
  `check_layers` 不受影响（ui 不进 core 头）
- core 变更 2 处，均只读/便捷查询性质：`auth_fsm_fail_streak()`（§1）、
  `user_face_set(user_id, face_id)`（§4）——**先改《技术路线规约》§5.2/§5.6
  再动代码**（纪律 3）
- CLAUDE.md 色彩铁律：横幅/扫描框全部取 `theme_color()`，无写死 hex

## 6. 性能预算（A7 单核）

| 项 | 预算 |
|---|---|
| 页面过渡 | 200ms；动画期间只 invalidate content 区（translate/opa 局部重绘） |
| 横幅 | 淡入 250ms / 停留 ≤2s / 淡出 250ms；横幅是 ~64px 高小条 |
| 按压 | 纯样式过渡（LVGL 内建 transition），无额外 anim 对象 |
| 禁用 | blur_backdrop、大 shadow_width（>20）、全屏波纹、`lv_obj_set_style_anim` |
| 验收 | `SAFE_PERF_LOG=1` 实测动画期主循环耗时；PC 基线 UI 27fps 不显著劣化；板子 ≥20fps |

## 7. 测试与验收

- ctest 7/7 保持全绿（UI 不进单测；auth_fsm 若补 fail_streak 接口，改 test_auth_fsm
  加 1 组断言）
- check_layers 全绿
- 端到端（socat + fm225_sim.py）截图验收：
  1. `no_match 1` → 横幅「未匹配（1/3）」黄 + 扫描框红闪 → 截图
  2. `no_match 3` → 横幅计数到 (3/3) → WAIT_OTP 动画切页 → 截图
  3. `liveness` → 红色 DANGER 横幅 + 扫描框红 → 截图
  4. `match 1` → 绿色「欢迎回来，{用户名}」+ 扫描框绿 + 开锁态主页过渡 → 截图
  5. `enroll 3` → 用户页录入入口全流程 → 成功横幅 → users.json face_id 写回核对
  6. 页面切换/按压录屏或连续截图对比
- 帧率留档（perf 数据回填进度文档）

## 8. 提交粒度（3 个独立 commit）

1. `ui1: 反馈中枢 ui_feedback + 横幅 + 扫描框状态染色`（§1 + §3 后半 + 规约 §5.2
   fail_streak 接口）
2. `ui2: 页面切换过渡 + 按压手感`（§2 + §3 前半）
3. `ui3: 人脸录入/删除 UI 入口`（§4）

每步：构建 0 error 无新增告警 + ctest 全绿 + 截图验收；进度文档按步回填。
