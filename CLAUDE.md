# CLAUDE.md — lv_port_linux（智能保险柜 UI / i.MX6ULL）

> 最近更新：2026-08-31（工程骨架重构：分层目录、模块化 CMake、HAL 后端注册、单元测试）
> 需求基线：`docs/01-需求规格.md`　架构说明：`docs/02-架构设计.md`　迁移记录：`docs/03-迁移清单.md`

## 1. 项目定位

- 多用户共享保险柜（民宿短租 / 小型办公室 / 多成员家庭），不是"家庭保险柜"。
  这一定位决定了多用户权限、TOTP、远程管理、审计日志、限时授权都是必需项。
- 基于 **LVGL v9**（submodule `lvgl/`）。
- 目标硬件：100ask i.MX6ULL（Cortex-A7 单核 / 512MB / 1024×600 触摸屏）。
- **当前阶段：PC 验证优先**。业务代码在 PC 与板子之间零改动迁移，差异只落在 HAL 与平台引导层。
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
│   ├── store/            存储门面：users.json / network.json / safe.log
│   │   └── credentials.c/h  保险柜开锁密码（password.cfg，与多用户 PIN 是两套）
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
docs/                     需求规格、架构设计、迁移清单、测试计划
data/                     PC 运行时数据（不入库）
legacy/                   旧实现，冻结，迁移完成前保留对照
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

## 6. 两套密码体系（易混）

- `core/store/credentials.c`：**保险柜开锁密码** `password.cfg`，支持虚位密码，认证加密存储。
- `core/store/store.c` 的 `user_*`：**多用户 PIN** `users.json`，PBKDF2-SHA256 哈希 + 每用户随机盐。

## 7. 安全红线

- PIN 只存 PBKDF2-SHA256 哈希 + 随机盐，不存明文、不做可逆加密。
- 敏感操作（增删用户、切换通道开关、改阈值、远程开锁）执行前需管理员 TOTP 二次确认。
- TOTP 一码一用（`last_otp_counter` 递增，防重放），±1 窗口容忍。
- 执行器脉冲上限 500ms 由 `hal_actuator_pulse()` 强制截断。
- 删除用户后必须仍保留至少 1 个启用管理员（`user_del` 内已校验）。
- 临时用户到期或次数用尽自动置为停用，不删除记录。

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
- 详细需求与验收见 `docs/01-需求规格.md`。
