# CLAUDE.md — lv_port_linux（智能保险柜 UI / i.MX6ULL）

> 最近更新：2026-08-27（补全目录结构与每个文件功能；修正路径 `Desktop`→`桌面`；PC 环境更正为 22.04；同步已知坑）

## 1. 项目定位
- Claude Code 运行在 Windows，通过 SSH 操作 Ubuntu 下的工程（`~/桌面/lv_port_linux`）。
- 基于 **LVGL v9**（submodule 指向 `lvgl` 主线 `release/v9.2`）的智能保险柜（safe）前端工程。
- PC 验证阶段（当前）聚焦四块：**日志、用户管理、数据存储、UI 设计**。
- **本期不做**：摄像头、真实开锁动作（主页"开锁"按钮仅占位/预留；`actuator` 在 PC 端只打印）。
- 硬件目标：NXP i.MX6ULL（百问网 100ask 板），LCD **1024×600 / RGB565**。
- PC 验证环境：Ubuntu **22.04** VM（book@192.168.150.139，内核 5.15），用 **SDL2 2.28.5** 桌面窗口；上板用 **FBDEV + EVDEV** 后端。
- 业务 `src/core` / `src/hal` / `src/ui` 代码在 PC 与板子之间**零改动**迁移（仅靠切换 defconfig + 编译器）。

## 2. 双平台构建
配置差异只在后端三行（PC=SDL；板子=FBDEV+EVDEV），其余（颜色深度16、日志、字体等）完全相同，由 `configs/*.defconfig` 决定，与源码无关。

**PC（看真实 UI 窗口，需进 VM 桌面）**
```bash
cd ~/桌面/lv_port_linux
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig        # 指向 SDL2 2.28.5
CMAKE=~/tools/cmake-3.22.1-linux-x86_64/bin/cmake
# 本机已 Ubuntu 22.04，系统 python3 为 3.10（>=3.7），无需再指定 Buildroot 的 python3
$CMAKE -B build_pc -DLV_PORT_DEFCONFIG=configs/pc.defconfig .
cmake --build build_pc -j$(nproc)
DISPLAY=:0 ./build_pc/bin/lvglsim
```

**开发板（i.MX6ULL，交叉编译 → ARM ELF）**
```bash
cd ~/桌面/lv_port_linux
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
export PATH=~/tools/cmake-3.22.1-linux-x86_64/bin:$BR/output/host/bin:$PATH
cmake -B build -DLV_PORT_DEFCONFIG=configs/get_started.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
  -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build -j$(nproc)            # 产物 build/bin/lvglsim（ARM ELF）
# 拷到板子根文件系统直接运行（NFS 挂载或 scp）
```
> ⚠️ 板子构建当前**不可用**：Buildroot 工具链 sysroot 不完整（缺 `crt1.o`/`libc.so`），需先在 SDK 侧修复（见 §6）。

## 3. 目录结构与每个文件功能

```
lv_port_linux/
├── CLAUDE.md                      # 本文件：项目说明、目录结构、约定、坑
├── DESIGN.md                      # 需求与设计说明（用户模型/日志/存储/UI 信息架构/主题色板）
├── CMake_VSCODE_修复记录.md        # VS Code/CMake 报错修复记录（目录改名、python3、源码红、tabbar 缺变量）
├── CMakeLists.txt                 # 主构建脚本：Kconfig 选 defconfig；业务源码 glob；零改动切 PC/板子
├── Kconfig                        # Kconfig 根：rsource lvgl/Kconfig + 3D truck demo 选项
├── manifest.json                  # LVGL 官方项目元数据（显示后端/色深下拉等）
├── LICENSE                        # 许可证
├── .clang-format                  # clang-format 代码格式配置
├── .gitignore                     # 忽略 .config / build/ / .deps / env
├── .gitmodules                    # lvgl 子模块（https://github.com/lvgl/lvgl.git）
├── .mcp.json                      # lvgl MCP（Kapa：https://lvgl.mcp.kapa.ai/），项目级
├── lv_port_linux.code-workspace   # VS Code 工作区设置（cmake 路径 / build_pc / configureOnOpen）
├── cmake/
│   └── user_cross_compile_setup.cmake   # 交叉编译工具链（Buildroot arm-buildroot-linux-gnueabihf；带 libevdev 的 sysroot）
├── configs/
│   ├── pc.defconfig               # PC 验证：SDL2 + FreeType 中文 + RGB565（默认）
│   ├── get_started.defconfig      # 开发板：FBDEV + EVDEV（交叉编译上板）
│   └── get_started_3d.defconfig   # 3D truck demo 全特性配置（参考/未启用）
├── src/
│   ├── main.c                     # 程序入口：注册/初始化显示后端 → lv_init → actuator_init → app_start → LVGL 主循环
│   ├── main.c.bak_204907          # main.c 的旧备份（勿用，历史残留）
│   ├── core/                      # 业务逻辑层（与平台无关，PC/板子零改动）
│   │   ├── config.c/.h            # 保险柜开锁密码：读/校验/写 password.cfg；含虚位密码（PIN_REAL_LEN=6, 虚位最大20）
│   │   ├── crypto.c/.h            # 认证加密原语：AES-CBC + 随机 IV + PBKDF2-HMAC-SHA256 + HMAC（password.cfg 用）
│   │   ├── store.c/.h             # 存储抽象层：users.json / network.json / safe.log（用户PIN哈希、网络psk、日志）
│   │   ├── unlock_backend.c/.h    # 解锁策略抽象：PIN 匹配任一启用用户；防暴力按用户独立计数
│   │   ├── worker.c/.h            # 通用异步 worker：后台线程执行阻塞任务 + 主线程结果泵（保证 LVGL 线程安全）
│   │   └── async_store.c/.h       # store 的类型化异步包装（配合 worker，回调回主线程，回调内可操作 LVGL）
│   ├── hal/                       # 硬件抽象层
│   │   └── actuator.c/.h          # 执行器：开锁机构高/低电平驱动（PC 端仅打印状态；板子接 GPIO）
│   ├── lib/                       # LVGL 移植框架（来自官方 lv_port_linux）
│   │   ├── backends.h             # 后端接口定义（display/indev backend 结构体与 init 函数原型）
│   │   ├── driver_backends.c/.h   # 多后端注册/初始化抽象（register / init_backend / is_supported / print）
│   │   ├── simulator_util.c/.h    # 仿真器工具（后端选择/窗口辅助等）
│   │   ├── simulator_settings.h   # 仿真器全局设置（默认窗口 1024×600，与目标屏一致）
│   │   ├── mouse_cursor_icon.c    # SDL 鼠标光标图标位图
│   │   ├── display_backends/      # 显示后端实现（按 defconfig 编译其中所需，其余不进产物）
│   │   │   ├── sdl.c              #   SDL2 窗口后端（PC）
│   │   │   ├── fbdev.c            #   帧缓冲 /dev/fb0 后端（开发板）
│   │   │   ├── drm.c             #   DRM/KMS 后端（可选）
│   │   │   ├── glfw3.c           #   GLFW 后端（可选）
│   │   │   ├── wayland.c         #   Wayland 后端（可选）
│   │   │   └── x11.c             #   X11 后端（可选）
│   │   └── indev_backends/       # 输入设备后端
│   │       └── evdev.c            #   触摸屏 evdev 后端（开发板；PC 不编译）
│   ├── ui/                        # 视图层（只做界面渲染 + 事件绑定；判定走 core，硬件动作走 hal）
│   │   ├── ui.c/.h                # UI 外壳：顶栏(锁/时钟/WiFi)+底部Tab+内容区；页面路由表；app_start/ui_switch_page
│   │   ├── theme.c/.h             # 主题系统：4 套色板 + 全局复用样式 + theme_switch 一键换肤
│   │   ├── fonts/                 # 字体
│   │   │   ├── fonts.h           #   字体统一入口 app_font(size)（PC=FreeType 动态 / 板子=嵌入位图）
│   │   │   ├── fonts_ft.c        #   FreeType 初始化（PC 从系统 Noto CJK 动态渲染中文，避缺字）
│   │   │   ├── lv_font_cn_14.c/.h（及 16/20/28）# 嵌入中文字体位图（开发板用）
│   │   │   └── lv_font_cn_decl.h #   位图字体声明
│   │   └── pages/                 # 各页面（CMakeLists 用 GLOB 编译，新增页面后重 Configure）
│   │       ├── page_monitor.c/.h  # 主页/综合监控页（锁状态·时钟·开锁占位·最近日志，PAGE_HOME）
│   │       ├── page_logs.c/.h     # 日志页（列表+筛选，PAGE_LOGS）
│   │       ├── page_settings.c/.h # 设置中枢（用户/网络/系统入口，进前二次验 admin PIN，PAGE_SETTINGS）
│   │       ├── page_users.c/.h    # 用户管理子页（列表/添加/改密/删除/启用，PAGE_USERS）
│   │       ├── page_network.c/.h  # 网络子页（WiFi 扫描/连接/PSK，PAGE_NETWORK）
│   │       ├── page_system.c/.h   # 系统子页（时间/安全策略/恢复出厂，PAGE_SYSTEM）
│   │       └── page_keypad.c/.h   # 开锁 PIN 键盘（全屏层，虚位密码校验，PAGE_KEYPAD）
│   └── safe/                      # 运行时数据目录（PC 阶段落此处免权限；板子 CMake 传 SAFE_DIR_DEVICE→/var/lib/safe）
│       ├── users.json             # 用户/角色/PIN哈希/盐（已建 admin/guest；板子权限 600 属主 root）
│       ├── safe.log               # 审计日志（JSON Lines，运行产生，滚动截断）
│       └── password.cfg           # 保险柜开锁密码（crypto 认证加密；首跑用默认 "123456"）
├── third_party/                   # 第三方库（自包含，无外部依赖）
│   ├── aes/                       # tiny-AES-c（AES 实现，crypto.c 调用）
│   │   └── aes.c/.h
│   └── sha256/                     # B-Con SHA256（PBKDF2/HMAC 用）
│       └── sha256.c/.h
├── build_pc/                      # PC(SDL) 构建产物（lvglsim + liblvgl_linux.a + compile_commands.json）
├── build_board/                   # 开发板交叉编译产物（当前 sysroot 不完整，暂不可上板）
├── .build_bak/                    # 旧构建目录备份（Desktop→桌面 改名时移入，可删）
├── .vscode/                       # VS Code 配置（c_cpp_properties / settings / tasks）
├── .cache/                        # clangd 索引缓存
└── .git/                          # git 仓库
```

> **两套密码体系（易混，注意区分）**
> - `core/config` + `core/crypto`：**保险柜开锁密码**（`password.cfg`），支持虚位密码，用认证加密存储。
> - `core/store` 的 `user_*`：**多用户 PIN**（`users.json`），存 PBKDF2-SHA256 哈希 + 每用户随机盐，绝不存明文。
> 二者用途与存储格式不同，不要混淆。

## 4. 代码约定
- **颜色集中管理**：所有颜色来自 `app_theme_t THEMES[]` + 一组可复用 `lv_style_t`（如 `st_screen/st_panel/st_text/st_accent_btn/st_border`）。页面控件只 `lv_obj_add_style(obj, &st_xxx, 0)`，**严禁在控件上写死 hex**。
- **主题切换**：`theme_switch(idx)` → `lv_obj_report_style_change()` 全局刷新，所有页面自动换肤。
- **已实现 4 套主题**：石墨黑 / 月白 / 蓝白 / 松石青。新增主题 = 在 `THEMES[]` 增一行填色板，无需改任何页面。
- **存储走抽象层**：业务只调 `user_*/net_*/log_*` 接口，底层今天是 JSON 文件，将来换 SQLite 或独立分区只改实现。
- **字体统一入口**：UI 代码只写 `app_font(14/16/20/28)`，PC 自动走 FreeType、板子自动走嵌入位图，不写死具体字体变量。
- **线程边界**：LVGL API 只能在主线程调用；阻塞任务（PBKDF2、文件 I/O、网络扫描）走 `worker_post`，结果在 `worker_poll()`（主线程）回调。

## 5. 安全红线（务必遵守）
- **用户 PIN**：只存 `PBKDF2-SHA256(密码 + 每用户随机盐)` 哈希，绝不存明文；验证时现算比对。
- **保险柜密码**：`password.cfg` 用 crypto 层的 AES-CBC 认证加密（随机 IV + 随机盐 + HMAC 完整性校验），相同明文每次密文不同。
- **WiFi psk**：用设备密钥（PC 阶段固定串，后续入安全元件）包裹加密存储；联网时 `net_get_psk()` 解密。
- **日志**：明文 JSON Lines（见 DESIGN.md），记录开锁/失败/用户变更/设置变更；追加写 + 滚动截断（防写爆）。
- **文件权限**：板子上 `users.json` / `network.json` 权限 600、属主 root，目录 `/var/lib/safe/`。

## 6. 已知坑
- **SDL2 必须 ≥ 2.0.12**（lvgl v9 SDL 后端用到 `SDL_PixelFormatEnum`），Ubuntu 源只给旧版；已源码编 **2.28.5** 装 `/usr/local`。PC 构建前务必 `export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig`。
- **Python3 解释器**：旧 VM(18.04) 系统 python3 仅 3.6 太低，曾硬编码 Buildroot python3.8；现 VM 为 **22.04**（系统 python3.10 够用），已删掉 `.vscode`/`code-workspace` 里的 `Python3_EXECUTABLE` 硬编码，PC 构建**无需**再指定（板子交叉编译仍需 Buildroot 的 python3）。
- **CMake 缓存绝对路径**：工程目录由 `~/Desktop` 改名 `~/桌面` 后，旧 `CMakeCache.txt` 里固化路径失效会拒绝配置。已删 `build_*` 重建（备份在 `.build_bak/`）。**不要再改名**；若必须迁移，记得删 `build_*` 重建。
- **SDL 双窗口**：SDL 后端会弹两个窗口（一个 UI、一个空白挡住 UI）；截图验证 UI 时需手动挪开空白窗口。
- **板子 sysroot 不完整**：Buildroot 工具链 `output/host/arm-buildroot-linux-gnueabihf/sysroot` 缺 `crt1.o`/`libc.so`，`build_board` 暂不可交叉编译上板。需先在 100ask SDK 重 make 一次 Buildroot 或恢复完整 sysroot。
- **改密键盘弹不出（待修）**：`lv_keyboard` 必须 `lv_keyboard_set_textarea(kb, ta)` 绑定到目标输入框，且键盘要建在**当前 screen / 顶层图层**上，否则不显示。
- **字体**：PC 已用 FreeType 从系统 Noto CJK 动态渲染解决缺字（`pc.defconfig` 开 `LV_USE_FREETYPE`）；开发板用嵌入位图 `lv_font_cn_*`，后续需补全完整字库。

## 7. 范围与边界
- 本期只做：设置 / 用户 / 网络 / 日志 四大块；真实开锁、摄像头不在范围内。
- 数据存储暂用 **JSON 文件 + 抽象层**，不引入数据库，不构建独立根文件系统。
- 详细需求与字段定义见同目录 **DESIGN.md**；CMake/VS Code 报错排查见 **CMake_VSCODE_修复记录.md**。
