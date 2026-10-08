# Claude Code 任务提示词：智能保险柜 UI 全面重设计（平板 / 现代化 / 图标化）

> 使用场景：直接粘贴给 Claude Code（Ubuntu VM 侧），在 `~/桌面/lv_port_linux` 上执行。
> 视觉参考：`E:\新建文件夹\ui_redesign_preview.html`（在浏览器打开即见，含点击切换页面的滑动+淡入过渡演示，是本次重设计的**视觉真源**）。
> 当前状态：已有 ui1~ui3 + 两 fix（交互增强），但视觉仍是旧风格（文字密集、图标少、底部 tab 栏）。本次要的是**真正的视觉重设计**。

---

## 0.0 已确认决策（用户 2026-09-13 裁定，Claude Code 直接遵循）

1. **图标落地 = 图标字体**：用 `lv_font_conv` 子集化开源线性图标（Lucide / Material Symbols，约 30~40 个）生成 `lv_font_t`，封装 `icon_label()` helper；不贴 PNG、不靠 emoji。预览 HTML 里的内联 SVG 仅作视觉真源，LVGL 侧不直渲 SVG。
2. **默认主题 = 浅蓝（index 4）**（2026-09-14 用户裁定保持浅蓝，见 `theme.c` 的 `DEFAULT_THEME_IDX`）：预览已默认按浅蓝展示；实现时 token 一律以 `app/ui/theme.c` 的 `THEMES[]` 为准，五套主题仍可切换、布局不变。
3. **SYSTEM 按钮折叠 bug 并入本轮修复**：旧版「恢复出厂设置」按钮被折叠线裁切；预览已把 SYSTEM 作为左 rail 第 7 项（i-sys）从布局规避，并须在实现中确保该按钮完整可见、可点。
4. **R9 自研 MQTT 由 Claude Code 实现（记录清楚、单独排期）**：当前 `app/core/remote/mqtt_client.c` 是 Paho MQTTAsync，板子缺 ARM Paho 自动 OFF 编进 `mqtt_stub.c` 显「MQTT 断线」。R9 设计见规约 §5 / §7.5 / §8.1（poll 状态机 + QoS1 重发 + 退避重连 + LWT + TLS 可插拔），实现后收敛到 4 线程模型。本文件 §10 给出记录与步骤。

---

## 0. 环境与硬约束（不可违背）

- **工程唯一真源**：`~/桌面/lv_port_linux`，分支 `refactor/skeleton`，HEAD=`1b9424d`。
- **目标设备**：i.MX6ULL，LCD **1024×600** 横屏，电容触摸 `/dev/input/event1`，FBDEV 32bpp。单核 A7 @528MHz，**LVGL v9**。
- **主题系统**：现有 token 化主题 **五套**（0 石墨黑 / 1 月白 / 2 蓝白 / 3 松石青 / 4 浅蓝），集中在 `app/ui/theme.c` + `app/ui/ui_anim.h`（动画常量）。**重设计必须沿用这套 token，不允许硬编码颜色**——保证五套主题仍可切换。
- **线程预算**：全机应用自建线程 **≤4**（main + worker + face + MQTT）。过渡动画**必须复用 LVGL 定时器 / `lv_anim`**，不得新建线程、不得每帧 `malloc`。
- **分层纪律**：ui 层只经 `event_bus` 订阅，不 include core 内部头（除已登记接口）；`tools/check_layers.sh` 必须通过。
- **禁止提交**：`CLAUDE.md`、`*.bak`、`.claude/`（用 `git add <精确文件>`，禁止 `git add -A`）。
- **性能基线**：HOME 稳态 ≥20fps（A7 实测曾 117fps PC / 板子无相机 25fps）；新增过渡不得显著劣化（>15% 判不通过）。

---

## 1. 设计目标（与参考 HTML 对齐）

| 维度 | 旧版 | 新版（本次） |
|---|---|---|
| 导航 | 底部 3~4 tab 栏 | **左侧垂直图标导航栏（rail）**，96~104px 宽，图标+小标签，激活项左侧 accent 条 |
| 图标 | 几乎纯文字 / 彩色圆+首字母 | **全量图标化**：导航、状态、卡片、列表行、按钮均带 SVG/图标字体图标 |
| 首页 | 信息平铺 | **卡片式仪表盘**：锁状态大卡 + 一键开锁 + 4 个统计卡 + 最近事件列表 |
| 过渡 | ui2 已有基础过渡 | **统一过渡系统**：页面切换 180~220ms，新页从右滑入+淡入、旧页淡出/左滑；ease 曲线统一 |
| 视觉 | 平面、描边为主 | **圆角卡片(16~20px) + 柔和阴影 + 留白 + 状态色点**（ok/warn/danger/blue） |
| 列表 | 纯文字行 | 头像色块 + 角色徽标 + 图标 + 次要信息小字 |

**参照 `ui_redesign_preview.html` 逐项实现**：左 rail 的 7 个入口（主页/用户/日志/人脸/网络/设置/系统）、首页 hero 卡 + 统计卡 + 事件列表、用户页头像+徽标列表。

---

## 2. 图标策略（最关键的实现点）

LVGL v9 默认**无图标字体、无 emoji**（Noto CJK 不含 emoji）。必须引入图标资源：

**推荐方案：嵌入图标字体（icon font）**
1. 取一套开源线性图标（如 Material Symbols Outlined / Lucide / Remix Icon），筛出本项目需要的约 30~40 个（锁、家、用户、列表、人脸、网络、设置、系统、指纹、铃铛、钥匙、加号、WiFi、电池、盾牌、相机、撤销、电源等）。
2. 用 LVGL 官方 `lv_font_conv`（或项目已有的字体工具链）把 SVG 转成 LVGL 字体（`lv_font_t`），生成 `app/ui/fonts/lv_font_icon_xxx.c`。
3. 封装 `icon_label(parent, ICON_LOCK, size)` 之类的 helper：内部 `lv_label_set_text(obj, LV_SYMBOL_...)` 或自定义符号映射表，统一走主题 token 上色。
4. 中文仍用现有 Noto Sans CJK FreeType；图标字体与中文字体并存（LVGL 支持同一 label 多字体 fallback 或分两个 label）。

**备选方案**：PNG/SVG 转 LVGL 图片 atlas（`lv_img` + `LV_IMG_CF_TRUE_COLOR_ALPHA`），适合需要彩色/复杂图标时。优先图标字体（体积小、可矢量缩放、易上色）。

**无论哪种，图标资源必须随构建编译进固件**（板子无网络），并在 `CMakeLists.txt` 登记。

---

## 3. 导航与页面切换系统（核心改造）

- 现有页面管理器改为支持 **左 rail 切换**：rail 是常驻控件，点击切换 `lv_screen_load` 的目标页。
- **过渡实现**（零每帧分配）：
  - 用 `lv_screen_load_anim(next, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false)`（或 `MOVE_LEFT` 按方向），或自建 `lv_anim` 对两个 screen 做 `x`/`opacity` 插值。
  - 曲线统一为 `lv_anim_set_path_cb(a, lv_anim_path_ease_out)`；时长常量进 `ui_anim.h`（如 `UI_TRANS_MS 200`）。
  - 禁止在过渡回调里 `lv_obj_create`/`free`（动画只改 transform/opacity）。
- 8 个页面（HOME/KEYPAD/LOGS/USERS/SETTINGS/FACE/SYSTEM/NETWORK）+ 弹窗（add_user/auth）全部接入统一过渡。
- 键盘/方向键导航（板子无物理键盘，但 PC 调试要走得通）可后补，先保证触摸。

---

## 4. 逐页面重设计要点（均对齐预览 HTML）

- **HOME**：锁状态 hero 卡（大锁图标 + 状态 + 最近开启时间 + 一键开锁大按钮带指纹图标）；4 统计卡（用户/事件/人脸/存储）每张带图标；最近事件列表（图标 + 主文案 + 灰色次文案 + 时间）。
- **USERS**：顶部「添加用户」按钮（带 + 图标）；列表行 = 头像色块(姓名首字) + 名称 + 灰色副信息 + 角色徽标（管理员/普通/临时，三色）+ 右侧时间/箭头。临时用户显式标注「限次/限时/过期」。
- **LOGS**：事件时间线，按类型图标着色（开锁/失败/人脸/系统），支持按类型筛选 chips。
- **FACE**：保留现有实时摄像头预览 + 四态扫描框（ui1 已实现），外框改圆角卡片；录入/删除入口图标化（ui3 已实现，重排样式）。
- **KEYPAD / SETTINGS / SYSTEM / NETWORK**：统一卡片 + 图标化表单项（开关项带图标、列表项带头像/状态点）；**SYSTEM 页「恢复出厂设置」按钮必须完整可见、可点（旧版被折叠线裁切，本轮一并修复）**——预览已把 SYSTEM 作为左 rail 第 7 项（i-sys）规避布局层面的折叠。
- **弹窗**：add_user / auth 统一圆角卡片 + 图标标题 + 按压态（ui2 已实现，重排）。

---

## 5. 主题与状态色

- 在 `theme.c` 现有 token 基础上，新增/微调：卡片表面色 `--surface-2/3`、阴影、状态色 `ok/warn/danger/blue`、圆角 `--r 18px`。
- 预览 HTML 用的是「深色 + 黄铜金 accent」方案；**若用户选定其它主题（如 Azure 蓝白浅色），改 token 即可，布局代码不变**。
- 状态色点（dot）用于顶栏与列表，颜色取自 token。

---

## 6. 实施步骤（建议拆分 commit）

1. `ui-redesign: 引入图标字体与 icon helper`（字体资源 + `icon_label` 封装 + CMake 登记）
2. `ui-redesign: 左 rail 导航替换底部 tab 栏`（导航控件 + 页面切换接入）
3. `ui-redesign: 统一页面过渡系统（200ms ease）`（`ui_anim.h` 常量 + `lv_screen_load_anim`）
4. `ui-redesign: HOME 仪表盘重设计`（hero 卡 + 统计卡 + 事件列表）
5. `ui-redesign: USERS/LOGS 列表图标化与卡片化`（逐页）
6. `ui-redesign: 其余页面（KEYPAD/SETTINGS/SYSTEM/NETWORK/FACE）卡片化` + 主题 token 微调
7. `ui-redesign: 文档同步`（规约 §5 导航/图标契约、开发进度新增里程碑）

> 每步先改《技术路线规约》对应小节（导航结构、图标契约、过渡参数）再动码。

---

## 7. 验收标准

| # | 项 | 方法 |
|---|---|---|
| 1 | PC 构建 0 error | `cmake -B build_pc ... && cmake --build build_pc -j4` |
| 2 | ctest 全绿 | `ctest --test-dir build_pc`（先 `pkill -f lvglsim`）7/7 |
| 3 | check_layers 通过 | `bash tools/check_layers.sh` |
| 4 | 视觉对齐预览 | PC(Xvfb) 截图 8 页 + 弹窗，与 `ui_redesign_preview.html` 逐项对比（布局/图标/卡片/圆角） |
| 5 | 过渡流畅 | 截过渡中间帧（t≈80ms）证明非瞬跳；HOME 稳态 fps 不降 >15% |
| 6 | 板子上板验证 | 交叉编译 → 推固件 → 8 页 + 弹窗 + 真实摄像头人脸页 渲染正常、过渡可感、无崩溃 |
| 7 | 主题可切换 | 五套主题切换后布局正常（不硬编码颜色） |
| 8 | 线程数 | 板子 `ls /proc/$(pidof lvglsim)/task|wc -l` 仍 ≤ 应用自建 4（过渡不增线程） |
| 9 | 现场恢复 | 验证后正式固件推回，S05lvgl 单实例、exe 非 deleted |

---

## 8. 已知坑（已踩过，避免重踩）

1. **`SAFE_TEST_SHOT` 截图列表空白假象**：用户/日志列表异步加载，截图时序会空白；判据靠 dd 抓 `/dev/fb0` 实屏（`dd if=/dev/fb0 of=/tmp/fb.raw bs=65536 count=38`，1024×600×4 BGRX，通道 [2,1,0] 转 RGB）。
2. **`lv_theme_create()` 无 parent 会丢默认控件样式** → 整页布局位移（ui2 教训）。新主题必须 `lv_theme_set_parent(新主题, 默认主题)`。
3. **过渡不要每帧 malloc**：动画只改 transform/opacity，回调里不 create/free。
4. **图标字体上色**：LVGL 图标字体 glyph 用 `img` 渲染时，颜色走 `style.text.color` 或 `style.img.recolor`；确认上色路径，避免图标全黑/全透明。
5. **Noto CJK 无 emoji**：不要试图用 emoji 字符当图标，必须用图标字体或图片。

---

## 9. 输出物

- 每步 commit hash + 一句话说明。
- PC 截图（8 页 + 弹窗）与预览 HTML 的对比说明。
- 板子验证截图（dd 实屏）与日志。
- 更新后的 `规约/技术路线规约.md`（导航结构、图标契约、过渡参数）与 `开发进度.md`。
- 最终汇报：是否对齐预览、fps 对照、线程数、现场恢复确认；**SYSTEM 折叠 bug 已在本轮修复（验收项）**；R9 自研 MQTT 排期与进度（见 §10）。

---

## 10. 后续任务：自研 MQTT 客户端（R9，由 Claude Code 实现）

**现状（已核实 2026-09-13）**：`app/core/remote/mqtt_client.c` 是 **Paho MQTTAsync**（`#include <MQTTAsync.h>`、`static MQTTAsync g_cli`），板子因缺 ARM 版 Paho 在构建时自动 OFF 编进 `mqtt_stub.c`，UI 显示「MQTT 断线」。**自研 MQTT 零代码落地**，仅规约设计。

**目标（规约 §5 / §7.5 / §8.1）**：用单线程 `poll()` 状态机替换 Paho，实现：
- QoS1 发布：缓存未确认报文，收到 PUBACK 才清；超时重发。
- 退避重连：指数退避 + 抖动，断线后自动重连并补发 LWT 清除。
- LWT（遗嘱）：连接时注册 `will`，异常掉线 broker 广播离线。
- TLS 可插拔：传输层抽象，局域网可关、远程可开（本期启用双向 TLS 见 FR-25）。
- 是收敛到「应用自建线程 ≤4（main+worker+face+MQTT）」的必要条件（Paho 实占 2 条 OS 线程，违反预算）。

**建议拆分 commit**：
1. `mqtt-r9: 抽象传输层 + poll 状态机骨架（连接/断开/重连）`
2. `mqtt-r9: QoS1 发布与 PUBACK 重发`
3. `mqtt-r9: LWT 与退避重连`
4. `mqtt-r9: TLS 可插拔 + 替换 Paho 接入点（保留 stub 作无网 fallback）`
5. `mqtt-r9: 板端启用 + 线程数验收（≤4）`

> R9 与 UI 重设计解耦，可独立推进；但两者都改 `app/core/remote/` 与 ui 状态展示，注意 `event_bus` 接线一致。
