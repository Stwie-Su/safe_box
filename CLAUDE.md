# CLAUDE.md — lv_port_linux（智能保险柜 UI / i.MX6ULL）

> 最近更新：2026-09-08（文档/计划/进度/资料路标更新；技术路线规约已升级 v1.3）
> 需求规约：`~/桌面/规约/需求规约.md`（v1.6）　技术路线：`~/桌面/规约/技术路线规约.md`（v1.3）　**当前计划与进度：见 §0，开工前必读**

## 0. 文档与资料位置（Ubuntu VM 内绝对路径，先读这里再动手）

| 类别 | 路径 | 说明 |
|---|---|---|
| **开发规约（做什么）** | `~/桌面/规约/需求规约.md` | v1.6，FR 需求条目与验收 |
| **开发规约（怎么做）** | `~/桌面/规约/技术路线规约.md` | v1.3：分层/线程模型/接口契约/风险与重构路线 |
| **开发计划** | `~/桌面/规约/Sprint3_Ubuntu先行计划.md` | 当前阶段执行计划（步骤 0–4、验收标准、纪律） |
| **开发进度** | `~/桌面/规约/开发进度.md` | §4「当前步指针」= 现在做到哪一步；**每完成一步必须更新** |
| **硬件资料** | `~/桌面/资料/FM22x系列人脸锁算法模组用户开发手册V1.7.pdf` | FM225 协议唯一依据（帧格式 / 命令字 / 校验） |
| 旧版归档 | `~/桌面/规约/归档/` | 历史版本规约 |

**工作纪律（铁律）**：
1. 动手前先读《技术路线规约》相关节 + 计划对应步骤；冲突时以规约为准并回改计划。
2. 按计划步骤顺序执行，**不跳步**；当前步骤**只能**从《开发进度.md》读（§4 指针 + §0 机器可读标记），不得凭记忆或猜。
3. 接口签名变更**先回改技术路线规约 §5，再改代码**。
4. 当前阶段**不上板**：不碰 FBDEV / Buildroot / 交叉编译 / 设备树。

### 0.1 进度维护（强制）——唯一进度真源 `~/桌面/规约/开发进度.md`

> 本工程有**两个开发端**共同推进：Windows 端 WorkBuddy 与 Ubuntu 端 Claude Code，
> 两端共用同一份进度文件。**谁改了代码不回写进度，另一端就会在错误的位置继续**——
> 这是本工程最高优先级的协作纪律。

**每次会话开始（必做，按顺序）**：

```bash
sed -n '1,20p' ~/桌面/规约/开发进度.md            # 读 §0 机器可读标记 + §4 当前步指针
sed -n '1,60p' ~/桌面/规约/Sprint3_Ubuntu先行计划.md # 读当前步骤的验收标准
```

1. 从 `CURRENT_STEP` / `STATUS` 确认现在做到哪一步；
2. 按计划文档该步骤的验收标准执行；
3. 若两处不一致，**以进度文件为准**并顺手修正计划文档。

**每完成一个步骤或子项（立即做，不要等会话结束）**：

1. 更新《开发进度.md》：§0 标记（CURRENT_STEP / STATUS / UPDATED / OWNER / NEXT）+ §4 指针表该步打 ✅ 并填实测结果（帧率、用例数、md5 等）+ 里程碑表追加一行；
2. 更新《Sprint3_Ubuntu先行计划.md》：对应步骤打 ✅ + 回填实测数据；
3. 独立 commit：`s3u<N>: <摘要>`（N = 步骤号）。

**每次会话结束（必做）**：

- 无论是否完成，都要在《开发进度.md》留下**可接续状态**：完成了哪一步、进行到哪个子项、当前阻塞是什么、下一步建议。
- **禁止只在对话里说"做完了"——没写进进度文件 = 没做完。**

## 1. 项目定位

- 多用户共享保险柜（民宿短租 / 小型办公室 / 多成员家庭），不是"家庭保险柜"。
  这一定位决定了多用户权限、TOTP、远程管理、审计日志、限时授权都是必需项。
- 基于 **LVGL v9**（submodule `lvgl/`）。
- 目标硬件：100ask i.MX6ULL（Cortex-A7 单核 / 512MB / 1024×600 触摸屏）。
- **当前阶段：Sprint3 Ubuntu 先行**（USB 摄像头 + socat 虚拟串口对，不上板；步骤与验收见 §0 计划）。业务代码在 PC 与板子之间零改动迁移，差异只落在 HAL 与平台引导层。
- 人脸模组 FM225、RTC DS3231 均未到货：接口与后端空壳已就位，业务链路用模拟后端完整验证。

## 2. 构建

配置差异全部由 defconfig 决定，源码不出现平台判断。

**PC（Ubuntu + SDL2）**
```bash
cd ~/桌面/lv_port_linux
cmake -B build_pc -DLV_PORT_DEFCONFIG=configs/pc.defconfig .
cmake --build build_pc -j$(nproc)
ctest --test-dir build_pc --output-on-failure      # 单元测试 + 分层检查

# 无桌面登录时用 Xvfb 提供虚拟显示
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 ./build_pc/bin/lvglsim
```

**开发板（交叉编译）**
```bash
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
export PATH=~/tools/cmake-3.22.1-linux-x86_64/bin:$BR/output/host/bin:$PATH
cmake -B build_board -DLV_PORT_DEFCONFIG=configs/board.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
  -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build_board -j$(nproc)
```
> 板子构建仍受 SDK sysroot 完整性影响；交叉编译时 MQTT 与测试默认关闭。

**可选特性**（`cmake/SafeFeatures.cmake`，缺依赖自动降级，不会让构建失败）

| 选项 | 默认 | 说明 |
|---|---|---|
| `SAFE_FEATURE_MQTT` | PC 自动探测 | 缺 paho-mqtt3a / cJSON 时编 `remote/mqtt_stub.c`、`rpc_stub.c` |
| `SAFE_FACE_BACKEND` | `fake` | `fake`（模拟，可注入分数）/ `fm225`（协议占位）/ `none` |
| `SAFE_BUILD_TESTS` | PC ON，交叉 OFF | CTest 用例 |
| `SAFE_DATA_DIR` | PC `data/`，板子 `/var/lib/safe` | 运行时数据目录 |

```bash
cmake -B build_pc -DSAFE_FEATURE_MQTT=OFF .     # 验证降级路径
```

**调试环境变量**

| 变量 | 作用 |
|---|---|
| `SAFE_DATA_DIR` | 覆盖数据目录 |
| `SAFE_TEST_PAGE` | 起始页：HOME/LOGS/SETTINGS/USERS/NETWORK/SYSTEM/KEYPAD |
| `SAFE_TEST_THEME` | 起始主题 0..3 |
| `SAFE_TEST_DLG` | 自动开弹窗：add_user / auth / change_pwd |
| `SAFE_TEST_SHOT` | 截图到指定路径（原始 RGB565）后退出 |
| `SAFE_PERF_LOG=1` | 每 2 秒打一行主循环耗时 |
| `SAFE_MQTT_HOST/PORT`、`SAFE_MQTT_OFF=1` | 远程通道参数与开关 |

## 3. 目录结构

```
app/                      业务代码
├── main.c                进程入口：引导 + 主循环，不含业务逻辑
├── app.c/h               应用编排：初始化顺序、周期节拍（不依赖 LVGL）
├── core/                 平台无关，禁止 lvgl.h / 平台头（tools/check_layers.sh 校验）
│   ├── err.h             统一错误码
│   ├── event_bus.c/h     事件总线（替代散落的回调字段）
│   ├── config.c/h        系统配置快照（阈值/MQTT/日志/数据目录）
│   ├── store/            存储门面：users.json / network.json / safe.log（唯一凭据体系）
│   ├── auth/             auth_fsm（置信度分级状态机）/ totp / unlock_backend
│   ├── remote/           mqtt_client + rpc（缺依赖时编 *_stub.c）
│   └── support/          worker、async_store、crypto、sha1
├── hal/                  硬件抽象：头文件 + 后端实现
│   ├── hal_face.h        ★ 人脸服务接口（业务层唯一可见）
│   ├── face/             face_service + backend_fake / backend_fm225 / backend_none
│   ├── time/             time_service + time_sys（DS3231 后端留位）
│   ├── actuator/         actuator_service + actuator_mock（GPIO 后端留位）
│   └── camera/           camera_service + camera_null（FR-7 预留）
├── ui/                   外壳、主题、字体、页面（ui_init 只做界面，不管业务编排）
└── platform/             platform_sdl / platform_fbdev / platform_null、perf_probe、debug_hooks
ports/lv_port/            LVGL 官方 Linux 移植层（原 src/lib）
third_party/              aes、sha256
tests/                    CTest：totp / auth_fsm / store / check_layers
tools/check_layers.sh     分层检查脚本
data/                     PC 运行时数据（不入库）
（docs/ 已外迁至 ~/桌面/规约/；legacy/ 已移出至 ~/桌面/_trash_20260831）
```

**构建产物**：`lvgl_linux`（移植层）、`safe_core` / `safe_hal`（业务静态库，不含 LVGL）、
`safe_ui`、`safe_platform`、`lvglsim`。

## 4. 分层规则（铁律）

1. `app/core/**` 不得出现 `lvgl.h`、`<linux/...>`、`SDL2`、`cJSON`（`remote/` 除外）。
2. `app/hal/*.h` 头文件只放类型与声明，不带平台头文件，避免污染上游。
3. `app/ui/**` 只做渲染与事件绑定；判定下沉到 core，硬件动作下沉到 hal。
4. `app/main.c` 只做引导；周期任务由 `app.c` 暴露纯 C 节拍函数，main 用 `lv_timer` 挂载。
5. 违反 1–4 会被 `ctest -R check_layers` 拦下。

## 5. 代码约定

- **颜色集中管理**：颜色一律来自 `THEMES[]` + `st_xxx` 样式，页面不写死 hex。切主题用 `theme_switch()`。
- **存储走抽象层**：业务只调 `user_*` / `net_*` / `log_*`，底层是 JSON 文件，将来换 SQLite 只改实现。
- **字体统一入口**：UI 只写 `app_font(14/16/20/28)`，PC 走 FreeType、板子走嵌入位图。
- **线程边界**：LVGL API 只在主线程；阻塞任务走 `worker_post`，结果在 `worker_poll()` 回调。
  跨线程事件用 `event_bus_post()`，主线程 `event_bus_pump()` 派发。
- **错误码**：模块间统一 `safe_err_t`；认证域保留 `auth_result_t`（映射 RPC 错误码）。

## 6. 密码体系（唯一凭据）

- **唯一凭据体系**：`core/store/store.c` 的 `user_*`——多用户 PIN `users.json`，PBKDF2-SHA256 哈希 + 每用户随机盐；虚位密码（FR-18）在 PIN 校验层实现（`pin_check_virtual`，计划步骤 1 落地），开锁可虚位、管理员二次确认必须精确匹配。
- **已删遗留**：`core/store/credentials.c/h`（旧「保险柜开锁密码」`password.cfg`，可逆加密反模式）已由 Sprint3 步骤 0 R3 删除——全工程零调用死代码，凭据体系统一到 store 的 PBKDF2 + 用户 PIN。

## 7. 安全红线

- PIN 只存 PBKDF2-SHA256 哈希 + 随机盐，不存明文、不做可逆加密。
- 敏感操作（增删用户、切换通道开关、改阈值、远程开锁）执行前需管理员 TOTP 二次确认。
- TOTP 一码一用（`last_otp_counter` 递增，防重放），±1 窗口容忍。
- 执行器脉冲上限 500ms 由 `hal_actuator_pulse()` 强制截断。
- 删除用户后必须仍保留至少 1 个启用管理员（`user_del` 内已校验）。
- 临时用户到期或次数用尽**直接删除记录（含人脸凭据）**（FR-9，需求 v1.6 定案）；当前代码仍为「置停用」，由计划步骤 1 对齐。

## 8. 接硬件时改哪里

| 硬件到货 | 改动位置 | 业务代码 |
|---|---|---|
| FM225 人脸模组 | `app/hal/face/backend_fm225.c`（补 UART 帧解析，标记 TODO-FM225），构建切 `-DSAFE_FACE_BACKEND=fm225` | 不动 |
| DS3231 RTC | 新增 `app/hal/time/time_rtc.c`，`time_service.c` 换默认后端 | 不动 |
| 电磁锁 / 继电器 | 新增 `app/hal/actuator/actuator_gpio.c`，`actuator_service.c` 换默认后端 | 不动 |
| 摄像头预览 | 新增 `app/hal/camera/camera_v4l2.c` | 不动（UI 预览区已预留） |

## 9. 已知坑

- **VM 桌面未登录时 `DISPLAY=:0` 不可用**，SDL 初始化失败会导致进程在 LVGL 内部崩溃（不是代码 bug）。
  用 `Xvfb :99` + `DISPLAY=:99`，或进桌面后再跑。
- **LVGL 内部宏（如 `LV_USE_SDL`）不保证传播到应用层**。平台能力判断用构建系统显式定义的
  `SAFE_PLATFORM_PC` / `SAFE_HAVE_SNAPSHOT`，不要依赖 LVGL 宏。
- `store_set_dir()` 会规范化尾斜杠；自己拼路径时仍要用 `store_dir()` 作为唯一来源。
- 板子息屏：内核 `consoleblank` 是只读参数，应用层用 `KDSETMODE KD_GRAPHICS` + `FBIOBLANK` 轮询保活，
  已实现在 `platform_fbdev.c`，别删。
- 板子性能：单核 A7，视频预览锁 320×240，MQTT 周期上报 5s，`SAFE_PERF_LOG=1` 可查主循环耗时。
- 改密键盘弹不出：`lv_keyboard` 必须 `lv_keyboard_set_textarea()` 绑定，且建在顶层图层。
- 字体：PC 用 FreeType 动态渲染 Noto CJK；板子用嵌入位图，字库需补全。

## 10. 范围与边界

- 本期（骨架重构期）做：目录分层、构建模块化、人脸接口预留与模拟后端、单元测试、PC 全链路验证。
- 本期不做：真实人脸采集、DS3231 驱动、视频解码链路、公网穿透、小程序、指纹/NFC/4G/CAN、数据库。
- 详细需求与验收见 `~/桌面/规约/需求规约.md`（v1.6）。

## 11. UI 矢量图标与「无单字图标」策略（2026-09-01）

为解决 v2 中用单字「锁/密/脸/用」占位的脏观感，重构为通用矢量图标：

- **`app/ui/icons.h` / `icons.c`**：13 种 SVG 风格矢量图标（复合 lv_obj），
  - 主页中央：圆环包住 + 完整锁头（图标 > 标识「锁」字）
  - 主页开锁卡：4×3 圆点键盘 / 4 角扫描 + 中央人脸
  - 主页信息卡：用户人头 + 肩部 / 列表行 + 圆点 / 信号 4 柱
  - 底部 Tab：房子 / 用户 / 三横线 / 齿轮
  - 页面：左箭头 / × / 小锁头（角标）
- **`ui_icon_set_color()`**：icon 子对象统一带 `UID_MAGIC` tag，主题切换回调可
  用 `lv_obj_tree_walk` 一次性重新染色（替换原先需列出每个控件的旧方式）。
- **3 / 4 / 4 角扫描框**直接用 mk_rect + outline 实现，无需 lv_canvas。
- **斜线 / ×** 使用 `lv_line` 而非旋转矩形（更可靠）。

## 12. 底部连续导航栏重构（2026-09-01）

旧设计：4 个独立胶囊按钮（accent 填充 + checked 高亮）。
新设计：`v3 单条白色面板 + 4 等宽 tab`，选中态用：
  - 上方一矩形 accent 蓝指示条 + 图标/文字 → accent 蓝
  - 取消 accent 胶囊（更克制、跟现代 UI 一致）
- 全屏覆盖层（KEYPAD / OTP / FACE）不切换高亮。
- 由 `tabbar_refresh_theme(idx)` 统一在 theme_switch 时刷新图标 / 文字颜色。

## 13. 人脸识别全屏页（PAGE_FACE，2026-09-01）

`page_face.c` / `page_face.h`（新增）：
- 路由：在 `ui.h` 的 `ui_page_t` 末尾追加 `PAGE_FACE`，注册路由
- 进入：从主页「人脸识别」卡片点击触发（`face_unlock_cb` → `ui_switch_page(PAGE_FACE)`）
- 当前阶段视频区留空（摄像头未配置），用 4 角扫描框 + 顶部光带动画（lv_anim）占位
- 提供占位文字「视频预览区域（摄像头未配置）」
- 底部状态文字「请将面部对准摄像头」+ 取消按钮（× 图标 + 关闭文字）
- 调试钩子：`SAFE_TEST_PAGE=FACE` 直接切到该页面

## 14. 主页（PAGE_HOME）v3 升级（2026-09-01）

- 中央圆环 + 矢量锁头图标（替代单字「锁」）
- 「最后一次开启：--」→「最后一次开启：昨天 18:30」（异步统计 fallback）
- 两张开锁卡：密码开锁 / 人脸识别 → 全部统一白色卡片样式（st_panel + accent 蓝图标）
- 三个信息卡：用户（图标 + 标题 + 值）/ 今日事件 / 网络，带具体默认值



## 15. UI v3.1 微调（2026-09-01 11:30 反馈）

按用户新提的 4 个反馈做的小调整：

1. **主页人脸卡 KEY_LOCKED 角标删除**：原先在「人脸识别」卡片右上角放的小锁头角标
   因为卡片本身是 flex column 布局，角标被 flex 流当成最后一列渲染到下方，
   与「模糊」副标签形成视觉重叠。直接删除角标，卡片恢复干净。
2. **人脸识别页返回按钮语义化**：从「小圆按钮 + < 矢量箭头」改成「图标 + "返回"
   文字按钮」，避免意义不明（用户误以为是「向前/取消」之类）。
3. **底栏 4 → 3 个 Tab**：「主页 / 日志 / 设置」（移除「用户」）；用户管理已
   收纳到「设置 → 用户管理」入口（page_settings 原有此入口，未变）。
   - `tab_index_of()`：PAGE_USERS 也归到「设置」高亮 (idx=2)
   - `TABS[3]`、`build_tabbar` 循环、`switch_page` 高亮循环均同步改为 3
4. **锁定 vs 解锁 颜色分离**：
   - 上锁（open=false）：`TH_DANGER` 红色，home 中央锁头图标、顶栏 . 已上锁
   - 开锁（open=true）：`TH_OK` 绿色，home 中央锁头图标变「开」字 + 顶栏 . 已开锁

## 16. 顺带修复（2026-09-01 11:30）

- `page_logs.c::s_summary` 创建后立刻 `lv_label_set_text(s_summary, "")`，
  避免 LVGL 默认 placeholder 文字「Text」显示在标题旁边（之前 v3 截图里能看到
  「日志记录 Text」，来自 LVGL label 默认占位）。

## 17. UI v3.2 反馈修正（2026-09-01 15:13）

按用户 15:13 的 2 条反馈做的关键修复：

### 17.1 修复「密码开锁后仍显示已上锁」bug

**根因**：v3.1 只用 `hal_actuator_state()` 判断开锁状态，但 hal_actuator_pulse(500)
是物理继电器脉冲（模拟器也是 500ms 后自动拉低），NFR-7 硬上限不变。500ms 之后
s_high=false，UI 立刻跳回「已上锁」。FSM 进入 UNLOCKED 状态（30s）这一路
语义被完全忽略。

**修复**：
- `app/core/auth/auth_fsm.c::TO_UNLOCKED` 从 3s 延长到 30s（语义窗口）；
- `app/ui/ui.c` 与 `app/ui/pages/page_monitor.c` 新增 `is_unlocked_now()`：
  ```c
  static inline bool is_unlocked_now(void) {
      return hal_actuator_state() || auth_fsm_state() == FSM_UNLOCKED;
  }
  ```
  status_timer_cb (500ms) 与 monitor_timer_cb (1000ms) 改用此函数，UI 显示 =
  物理脉冲 OR FSM 30s 窗口，谁先到谁就位。

### 17.2 顶栏右侧 WiFi / MQTT 显式显示

**变更**：
- `app/ui/ui.c::build_topbar` 右侧从「1 点 + 1 WiFi 符号」改为两个独立 chip：
  - `[WiFi icon] WiFi 5G` 浅灰圆角背景
  - `[dot] MQTT 已连 / 断线` 浅灰圆角背景
- 文字通过 `s_wifi_text` / `s_mqtt_text` 句柄，status_timer_cb 500ms 实时刷新；
- 颜色：MQTT 断线时文字变 TH_DANGER 红色；WiFi 在 PC 阶段默认假定已连（绿色 icon），
  真机接 `wifi_is_connected()` 后可换接口。

### 17.3 测试增强（PC only）

`app/platform/debug_hooks.c::take_shot`：
- 加 `SAFE_TEST_UNLOCK=1` 环境变量触发：截屏前自动调用 `auth_fsm_note_unlock("admin")`
  模拟一次成功开锁链路（hal_actuator_pulse + FSM=UNLOCKED）；
- 加 `usleep(1200*1000)` 让 status_timer_cb (500ms) + monitor_timer_cb (1000ms)
  在 take_shot 内真实跑一次，避免截图时 UI 状态滞后。

截图验证（1280×720）：
- `v32_final_locked.png`：中央红锁头 + 「保险柜已上锁」+ 顶栏 ●已上锁（红）
- `v32_final_unlocked.png`：中央绿锁头 + 「保险柜已开启」+ 顶栏 ●已开锁（绿）+
  MQTT 已连（绿）
