# CLAUDE.md — lv_port_linux（智能保险柜 UI / i.MX6ULL）


## 1. 项目定位
- claude code运行在windows，通过ssh操作ubantu下的工程。
- 基于 **LVGL v9** 的智能保险柜（safe）前端工程。PC 验证阶段（当前）只聚焦四块：**日志、用户管理、数据存储、UI 设计**。
- **本期不做**：摄像头、开锁功能（主界面"开锁"按钮仅占位/预留）。
- 硬件目标：NXP i.MX6ULL（百问网 100ask 板），LCD **1024×600 / RGB565**。
- PC 验证环境：Ubuntu 18.04 VM（book@192.168.150.139），用 **SDL2 2.28.5** 桌面窗口；上板用 **FBDEV + EVDEV** 后端。
- 业务 ui/core 代码在 PC 与板子之间**零改动**迁移（仅靠切换 defconfig + 编译器）。
- 存在的问题：1.目前无法跑通ui，字体有问题，风格按照design.md设计。2.cmake构建完以后生成了两个窗口（一个ui，另一个空白窗口正好挡住ui），如果要截图验证ui，记得挪开空白的窗口。
## 2. 双平台构建
配置差异只在后端三行（PC=SDL；板子=FBDEV+EVDEV），其余（颜色深度16、日志、字体等）完全相同。

**PC（看真实 UI 窗口，需进 VM 桌面）**
```bash
cd ~/Desktop/lv_port_linux
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig        # 指向 SDL2 2.28.5
CMAKE=~/tools/cmake-3.22.1-linux-x86_64/bin/cmake
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
$CMAKE -B build_pc -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build_pc -j$(nproc)
DISPLAY=:0 ./build_pc/bin/lvglsim
```

**开发板（i.MX6ULL，交叉编译 → ARM ELF）**
```bash
cd ~/Desktop/lv_port_linux
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
export PATH=~/tools/cmake-3.22.1-linux-x86_64/bin:$BR/output/host/bin:$PATH
cmake -B build -DLV_PORT_DEFCONFIG=configs/get_started.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
  -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build -j$(nproc)            # 产物 build/bin/lvglsim（ARM ELF）
# 拷到板子根文件系统直接运行（NFS 挂载或 scp）
```

## 3. 目录结构（约定）
```
src/ui/            页面与组件（主页 / 设置 / 用户管理 / 网络 / 系统 / 日志）
src/lib/store/     存储抽象层（user / net / log 读写，业务不感知底层格式）
src/lib/theme/     主题系统与调色板（theme.h / theme.c）
configs/           pc.defconfig  /  get_started.defconfig
```

## 4. 代码约定
- **颜色集中管理**：所有颜色来自 `app_theme_t THEMES[]` + 一组可复用 `lv_style_t`（如 `st_screen/st_panel/st_text/st_accent_btn/st_border`）。页面控件只 `lv_obj_add_style(obj, &st_xxx, 0)`，**严禁在控件上写死 hex**。
- **主题切换**：`theme_switch(idx)` → `lv_obj_report_style_change()` 全局刷新，所有页面自动换肤。
- **已实现 4 套主题**：石墨黑 / 月白 / 蓝白 / 松石青。新增主题 = 在 `THEMES[]` 增一行填色板，无需改任何页面。
- **存储走抽象层**：业务只调 `user_*/net_*/log_*` 接口，底层今天是 JSON 文件，将来换 SQLite 或独立分区只改实现。

## 5. 安全红线（务必遵守）
- **用户 PIN**：只存 `PBKDF2-SHA256(密码 + 每用户随机盐)` 哈希，绝不存明文；验证时现算比对。
- **WiFi psk**：`AES-256-CBC` 用设备密钥包裹存为 `psk_enc`；系统联网时 `net_get_psk()` 解密。PC 阶段设备密钥用固定串，后续入安全元件。
- **日志**：明文 JSON Lines（见 DESIGN.md），记录开锁/失败/用户变更/设置变更；追加写 + 滚动截断（防写爆）。
- **文件权限**：`users.json` / `network.json` 权限 600、属主 root，目录 `/var/lib/safe/`（PC 阶段直接落在 rootfs 该目录）。

## 6. 已知坑
- SDL2 必须 **≥ 2.0.12**（lvgl v9 SDL 后端用到 `SDL_PixelFormatEnum`），Ubuntu 18.04 源只给 2.0.8；已源码编 **2.28.5** 装 `/usr/local`（`./configure --prefix=/usr/local --enable-video-x11=yes`）。
- VM 自带 cmake 3.10.2 太老、python3.6 太低；统一用 `~/tools/cmake-3.22.1` + Buildroot 的 `python3.8.5`。
- VM apt 镜像 DNS 失效，`libsdl2-dev`/`build-essential` 等处于 `iU` 破损态——不影响 native/交叉编译。
- **改密键盘弹不出 bug（待修）**：`lv_keyboard` 必须 `lv_keyboard_set_textarea(kb, ta)` 绑定到目标输入框，且键盘要建在**当前 screen / 顶层图层**上，否则不显示。

## 7. 范围与边界
- 本期只做：设置 / 用户 / 网络 / 日志 四大块；开锁、摄像头不在范围内。
- 数据存储暂用 **JSON 文件 + 抽象层**，不引入数据库，不构建独立根文件系统。
- 详细需求与字段定义见同目录 **DESIGN.md**。
