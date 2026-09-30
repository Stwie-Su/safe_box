# CLAUDE.md — lv_port_linux（智能保险柜 / i.MX6ULL + LVGL v9）

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **规约驱动开发**：需求规约（做什么）→ 技术路线规约（怎么做）→ 开发进度（做到哪）三文档是本仓库唯一权威，全部在 `规约/` 下（**随仓库入库**，2026-09-30 起）。`资料/`（厂商 FM225 手册）**不入库、仅本地存在**。开工前必读三文档。

## 1. 文档地图（相对仓库根，均可直接打开）

| 类别 | 路径 | 说明 |
|---|---|---|
| 需求规约（做什么）**v1.10** | `规约/需求规约.md` | 需求变更先改这里再动代码 |
| 技术路线规约（怎么做）**v1.5** | `规约/技术路线规约.md` | 接口签名变更先回改 §5 |
| **开发进度（唯一真源）** | `规约/开发进度.md` | 当前步只从 §0 读，不得凭记忆猜 |
| 当前执行计划 | `规约/计划/下一步行动计划_2026-09-18.md` | R9 主线 + 缺陷修复执行计划 |
| MQTT 专项进度 | `规约/专项/R9_MQTT_进度.md` | R9 T01~T06 落地明细 |
| R9 T03/T04 设计分解 | `规约/专项/R9_T03_T04_设计与任务分解_2026-09-20.md` | 五模块拆解与任务书 |
| UI 重设计任务书 | `规约/专项/claude_prompt_ui_redesign.md` | M1~M6（M6 四主题适配未完） |
| FM225 上板接线与自检 | `规约/参考/FM225_上板接线与自检清单.md` | 上板联调必读 |
| UI 视觉真源 | `规约/参考/ui_redesign_preview_v2.html` | 1024×600，浏览器打开 |
| Debug 案例集 | `规约/案例/` | D 系列缺陷复盘两册 + 现状审查 |
| 旧版归档 | `规约/归档/` | 历史快照**不维护、不改写** |
| FM225 手册（协议唯一依据，不入库） | `资料/FM22x系列人脸锁算法模组用户开发手册V1.7.pdf` | 仅本地开发环境有 |
| 环境配置 / 排错（项目外） | `~/桌面/配置笔记/` | 与代码无关的环境问题 |

## 2. 工作纪律（铁律）

1. 动手前先读《技术路线规约》相关节 + 计划当前步骤；冲突以规约为准并回改计划。
2. 按计划步骤顺序执行**不跳步**；当前步骤**只能**从《开发进度.md》读（§0 标记 + §4 指针），不得凭记忆猜。
3. 接口签名变更**先回改技术路线规约 §5，再改代码**。
4. 板上工作**受控进行**：板上构建 `build_board`（见 §5）与 FM225 真模组 USB 联调已解锁；仍不碰设备树 / Buildroot 定制，除非行动计划明确安排。

## 3. 进度维护（强制，两端共用）

Windows 端 WorkBuddy 与 Ubuntu 端 Claude Code 共用同一份进度文件，**没写进进度文件 = 没做完**。

**会话开始（必做）**：

```bash
sed -n '1,20p' 规约/开发进度.md                        # §0 标记 + §4 当前步指针
sed -n '1,60p' 规约/计划/下一步行动计划_2026-09-18.md    # 当前阶段执行计划与拍板决策
```

不一致时以进度文件为准，并顺手修正计划文档。

**每完成一步 / 子项（立即，别等会话结束）**：更新《开发进度.md》（§0 标记 + §4 打 ✅ 填实测数据 + 里程碑一行）→ 更新行动计划文档该步骤 ✅ → 独立 commit（沿用 `feat:/fix:` 风格）。

**会话结束（必做）**：留下可接续状态——做到哪一步、进行到哪个子项、当前阻塞、下一步建议。

## 4. 项目定位

多用户共享保险柜（民宿短租 / 小型办公室 / 多成员家庭）→ 多用户权限、TOTP、远程管理、审计日志、限时授权均为必需项。基于 **LVGL v9**；目标硬件 100ask i.MX6ULL（Cortex-A7 单核 / 512MB / 1024×600 触摸屏）。

**当前阶段：R9 自研 MQTT 已收口**（《开发进度.md》§16：协议层 codec/inflight/fsm 带确定性单测 + tls_stream(mbedTLS) + rpc 十指令 + 审计对齐，16 缺陷全修；`ctest` 17/17）。**下一步 = 上板实测（FR-15，P0 最大缺口）→ OTA 期①（FR-29，PC 可验的固件包接收/校验/落盘）**；**板子未接入是当前唯一阻塞项**。RTC 后端抽象已就位（`SAFE_TIME_BACKEND=sys|rtc`），硬件对接状态见《开发进度.md》。

## 5. 构建

配置差异全由 defconfig 决定，源码不出现平台判断。

```bash
# PC（Ubuntu + SDL2）
cd ~/桌面/lv_port_linux
cmake -B build_pc -DLV_PORT_DEFCONFIG=configs/pc.defconfig .
cmake --build build_pc -j$(nproc)
ctest --test-dir build_pc --output-on-failure

# PC + MQTT over TLS（build_tls = pc.defconfig + 双开关）
cmake -B build_tls -DLV_PORT_DEFCONFIG=configs/pc.defconfig \
  -DSAFE_FEATURE_MQTT=ON -DSAFE_FEATURE_MQTT_TLS=ON .
cmake --build build_tls -j$(nproc)

# 无桌面登录时（DISPLAY=:0 不可用）
Xvfb :99 -screen 0 1280x800x24 & DISPLAY=:99 ./build_pc/bin/lvglsim

# 跑单个测试
ctest --test-dir build_pc -R test_fm225_proto --output-on-failure

# FM225 真模组启动器（单实例守卫 + by-id 稳定串口 + 相机自动扫描）
./run_fm225.sh

# 开发板（交叉编译，产物 build_board/bin/lvglsim）
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
export PATH=~/tools/cmake-3.22.1-linux-x86_64/bin:$BR/output/host/bin:$PATH
cmake -B build_board -DLV_PORT_DEFCONFIG=configs/board.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
  -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build_board -j$(nproc)
```

可选特性（`cmake/SafeFeatures.cmake`，缺依赖自动降级、不会让构建失败）：`SAFE_FEATURE_MQTT`（PC 自动探测）/ `SAFE_FEATURE_MQTT_TLS` / `SAFE_FACE_BACKEND=fake|fm225|none` / `SAFE_BUILD_TESTS`（PC ON、交叉 OFF）/ `SAFE_DATA_DIR`（PC `data/`，板子 `/var/lib/safe`）。

调试环境变量（`grep -r 'getenv("SAFE_' app` 为准）：`SAFE_DATA_DIR`、`SAFE_TEST_PAGE`（HOME/LOGS/SETTINGS/USERS/NETWORK/SYSTEM/KEYPAD/FACE）、`SAFE_TEST_THEME`(0..4，THEME_COUNT=5)、`SAFE_TEST_DLG`(add_user|auth|change_pwd)、`SAFE_TEST_SHOT` / `SAFE_TEST_SHOT_MS`、`SAFE_TEST_UNLOCK=1`、`SAFE_TEST_TRANS_MS` / `SAFE_TEST_TRANS_PAGE`、`SAFE_TEST_FACE`、`SAFE_ICON_CHECK`、`SAFE_PERF_LOG=1`、`SAFE_MQTT_HOST|PORT`、`SAFE_MQTT_OFF=1`、`SAFE_FACE_BACKEND` / `SAFE_FACE_DEV`、`SAFE_FM225_DEV`（真模组串口，建议 by-id 路径）、`SAFE_FACE_PREVIEW_SCALE`、`SAFE_CAMERA_BACKEND` / `SAFE_CAMERA_DEV` / `SAFE_CAMERA_ROT`、`SAFE_TIME_BACKEND` / `SAFE_RTC_DEV`。

辅助脚本：`tools/fm225_sim.py`（socat 虚拟串口对扮模组）、`tools/fm225_selftest.py`（不启主程序的串口链路 30 秒自检）、`tools/crash_consistency_test.sh`（store 崩溃一致性长跑）、`tools/mqtt_lab.py`（MQTT 本地实验）、`tools/check_layers.sh` / `tools/check_main_thread_store.sh`（分层与线程纪律闸门）。

## 6. 目录结构

```
规约/                规约驱动开发文档（入库）：需求 / 技术路线 / 开发进度
│                    + 计划/ 专项/ 案例/ 参考/ 归档/
资料/                厂商手册（不入库，仅本地）
app/
├── main.c          进程入口：引导 + 主循环，不含业务逻辑
├── app.c/h         应用编排：初始化顺序、周期节拍（不依赖 LVGL）
├── core/           平台无关，禁 lvgl.h / 平台头（tools/check_layers.sh 校验）
│   ├── err.h / event_bus / config
│   ├── store/      users.json / network.json / safe.log（唯一凭据体系）
│   ├── auth/       auth_fsm / totp / unlock_backend
│   ├── remote/     mqtt_client(codec/inflight/fsm/tls_stream) + rpc（缺依赖时编 *_stub.c）
│   └── support/    worker、async_store、crypto、sha1
├── hal/            硬件抽象：头文件 + 后端实现
│   ├── hal_face.h  ★ 人脸服务接口（业务层唯一可见）
│   ├── face/       face_service + backend_fake / fm225 / none
│   ├── time/  actuator/  camera/
├── ui/             外壳、主题、字体、页面（ui_init 只做界面，不管业务编排）
└── platform/       platform_sdl / fbdev / null、perf_probe、debug_hooks
ports/lv_port/      LVGL 官方 Linux 移植层
third_party/        aes、sha256、stb
tests/              CTest（17 项：store 缓存/崩溃/P0、mqtt codec/fsm/inflight、rpc、
                    fm225_proto/enroll、auth_fsm、cred_reconcile、totp、unlock_backend、
                    event_bus + check_layers、check_main_thread_store）
tools/              自检 / 实验 / 字体再生成脚本
configs/            pc / board / board_3d 三套 defconfig
cmake/              特性与版本模块 + 交叉编译工具链
data/               PC 运行时数据（不入库）
```

产物：`lvgl_linux`、`safe_core` / `safe_hal`（业务静态库，不含 LVGL）、`safe_ui`、`safe_platform`、`lvglsim`。

## 7. 分层规则（铁律，违反会被 `ctest -R check_layers` 拦下）

1. `app/core/**` 禁 `lvgl.h`、`<linux/...>`、`SDL2`、`cJSON`（`remote/` 除外）。
2. `app/hal/*.h` 只放类型与声明，不带平台头文件。
3. `app/ui/**` 只做渲染与事件绑定；判定下沉 core，硬件动作下沉 hal。
4. `app/main.c` 只做引导；周期任务由 `app.c` 暴露纯 C 节拍函数，main 用 `lv_timer` 挂载。

## 8. 代码约定

- **颜色**：一律来自 `THEMES[]` + `st_xxx` 样式，页面不写死 hex；切主题用 `theme_switch()`。
- **存储**：业务只调 `user_*` / `net_*` / `log_*`，底层是 JSON，将来换 SQLite 只改实现。
- **字体**：UI 只写 `app_font(14/16/20/28)`；PC 走 FreeType、板子走嵌入位图。
- **线程**：LVGL API 只在主线程；阻塞任务走 `worker_post`，结果在 `worker_poll()` 回调；跨线程事件用 `event_bus_post()`，主线程 `event_bus_pump()` 派发。
- **错误码**：模块间统一 `safe_err_t`；认证域保留 `auth_result_t`（映射 RPC 错误码）。
- **图标**：统一 `app/ui/icons.h`（矢量复合 lv_obj）；子对象带 `UID_MAGIC` tag，主题切换用 `lv_obj_tree_walk` 一次性染色。斜线 / × 用 `lv_line`，不用旋转矩形。
- **开锁状态判定**：`is_unlocked_now() = hal_actuator_state() || auth_fsm_state() == FSM_UNLOCKED`（物理脉冲 OR FSM 30s 窗口）。**不能只用 actuator_state**——脉冲 500ms 后拉低会误判回「已上锁」。
- **LVGL label**：创建后立刻 `lv_label_set_text(x, "")`，否则默认占位「Text」会露出。

## 9. 安全红线

- PIN 只存 PBKDF2-SHA256 哈希 + 随机盐，不存明文、不做可逆加密。（旧 `credentials.c/h` 已删，凭据体系统一到 store 的 `user_*`）
- 敏感操作（增删用户、通道开关、改阈值、远程开锁）执行前需管理员 TOTP 二次确认。
- TOTP 一码一用（`last_otp_counter` 递增防重放），±1 窗口容忍。
- 执行器脉冲上限 500ms 由 `hal_actuator_pulse()` 强制截断。
- 删除用户后必须仍保留 ≥1 个启用管理员（`user_del` 内已校验）。
- 临时用户到期 / 次数用尽**直接删除记录（含人脸凭据）**（FR-9，需求 v1.6 定案）；实现对齐状态以《开发进度.md》为准。

## 10. 接硬件时改哪里

| 硬件到货 | 改动位置 | 业务代码 |
|---|---|---|
| FM225 人脸模组 ✅已接入 | `app/hal/face/backend_fm225.c` + `fm225_proto.c`（UART 帧解析）+ `-DSAFE_FACE_BACKEND=fm225`；剩余缺口 N1~N5 见《开发进度.md》§13.2 | 不动 |
| 摄像头预览 ✅已接入 | `app/hal/camera/camera_v4l2.c`（UVC 自动扫描 + MJPG，旋转用 `SAFE_CAMERA_ROT`） | 不动 |
| DS3231 RTC | `app/hal/time/time_rtc.c` 壳已在（`SAFE_RTC_DEV`），到货后实测对接 | 不动 |
| 电磁锁 / 继电器 | 新增 `app/hal/actuator/actuator_gpio.c`，`actuator_service.c` 换默认后端 | 不动 |

## 11. 已知坑

- **读取纪律：软拦截优先；硬拦截只兜底"读了也没用"的纯二进制**
  - **硬拦截**（项目级 `.claude/settings.json` → `permissions.deny`，仅在本项目生效）：只拦**永远不需要人看**的生成物与二进制 —— `*.otf` `*.ttf` `*.woff` `*.pack` `*.a` `*.o` `*.d` `*.internal` `*.make` `*.pyc`、`build_pc/bin/**`，以及 `Grep(build_pc/**)`。这些文件读了只会撑爆上下文，且得不到任何信息。
  - **软拦截 1 · 自家字体** `app/ui/fonts/lv_font_cn_*.c`（14/16/20/28 号，160KB~449KB，共 ~1.08MB）：**非 bug 不读**。只有在排查"缺字 / 方框 / 字库不全"这类**字体本身**的问题、且已无其他办法时才可以读，并且**只能**：`sed -n '1,60p'` 看文件头（字模表结构、glyph 范围、bpp），或 `wc -l` / `grep -c` 做统计。**严禁整文件 Read / Grep** —— 读一个 460KB 字体约 12 万 token，必然触发 autocompact thrashing。
  - **软拦截 2 · lvgl 大源码**（字体 `.c` 632KB~1.3MB、glad、thorvg、lodepng、vg_lite，以及巨型头 `glad/gl.h` 420KB、`stb_image.h` 304KB）：官方代码不是 bug 来源，**查 API 用 grep 定位符号、只读相关片段**，不要整文件通读 `.c` 或巨型 `.h`。
  - **软拦截 3 · 构建产物** `build_pc/`（`compile_commands.json` 410KB、`DependInfo.cmake` 103KB）：clangd 自己会读，**不要用 Read 打开**；要查编译选项用 grep 或脚本抽取。
- VM 桌面未登录时 `DISPLAY=:0` 不可用，SDL 初始化失败会在 LVGL 内部崩溃（非代码 bug）→ 用 `Xvfb :99` + `DISPLAY=:99`。
- LVGL 内部宏（如 `LV_USE_SDL`）不保证传播到应用层 → 用构建系统显式定义的 `SAFE_PLATFORM_PC` / `SAFE_HAVE_SNAPSHOT`。
- `store_set_dir()` 会规范化尾斜杠；拼路径仍以 `store_dir()` 为唯一来源。
- 板子息屏：内核 `consoleblank` 是只读参数，应用层用 `KDSETMODE KD_GRAPHICS` + `FBIOBLANK` 轮询保活（已实现在 `platform_fbdev.c`，别删）。
- 板子性能：单核 A7，视频预览锁 320×240，MQTT 周期上报 5s，`SAFE_PERF_LOG=1` 查主循环耗时。
- 改密键盘弹不出：`lv_keyboard` 必须 `lv_keyboard_set_textarea()` 绑定，且建在顶层图层。
- 字体：PC 用 FreeType 动态渲染 Noto CJK；板子用嵌入位图，字库需补全。

## 12. 范围与边界

- 本期做：R9 自研 MQTT ✅ 已收口（《开发进度.md》§16：codec/inflight/fsm + tls_stream(mbedTLS) + rpc 十指令）；**FR-29 OTA 两期**（期① 固件包接收/校验/落盘 PC 可验；期② 分区切换/自确认/回滚，强依赖上板）、FM225 录入/解锁闭环（N1~N3 完成度以代码核对为准）、FR-21 凭据一致性、FR-23 模组健康降级、D10 hal_net + NETWORK 页、DS3231 驱动、电磁锁 GPIO 后端、单元测试、板上验证。
- 本期不做：公网穿透、小程序、指纹/NFC/4G/CAN、数据库（SQLite 留接口）。
