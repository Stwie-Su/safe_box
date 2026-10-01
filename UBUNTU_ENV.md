# Ubuntu 开发主机环境参考（AI Agent 速查）

> 用途：本文档集中记录 **Ubuntu 开发主机（192.168.150.139）侧** 的工具链、SDK 路径、
> 设备树路径与构建命令，供 AI Agent 在修改 / 构建 / 上板本工程（`safe_box` / `lv_port_linux`，
> i.MX6ULL + LVGL v9 智能保险柜）时快速定位资源。
>
> 配套活文档：`CLAUDE.md`（agent 入口 / 文档地图）、`规约/`（需求 / 技术路线 / 进度）。
> 本文档只覆盖「Ubuntu 主机环境」，不重复业务规约内容。
>
> 实测日期：2026-10-01。路径均为该 VM 上的真实绝对路径，**不要用 `~` 之外的相对路径**。

---

## 1. 主机与连接方式

| 项 | 值 |
|----|----|
| VM IP | **192.168.150.139**（静态，netplan + systemd-networkd） |
| 登录用户 | **book** / 密码 **123456**（SSH 已免密） |
| 系统 | Ubuntu 22.04.5 LTS（宿主机内核 5.15.0-191，x86_64） |
| 桌面路径 | **`~/桌面`**（中文目录，不是 `~/Desktop`） |
| 项目根 | **`~/桌面/lv_port_linux`**（`/home/book/桌面/lv_port_linux`） |
| 当前分支 / HEAD | `main` / `2519cf4`（2026-09-30 工程整理） |
| root 状态 | **不可用**（`sudo` 无 setuid、`su root` 认证失败）→ 不要尝试 root 操作 |

⚠️ **两镜像共用 192.168.150.139**：旧 100ask VM 与新 VM 同 IP，**同时只开一台**，否则 IP 冲突。

---

## 2. SDK 与交叉工具链（Toolchain）

### 2.1 SDK 根目录
```
/home/book/100ask_imx6ull-sdk
├── Buildroot_2020.02.x/      # Buildroot（生成板子 rootfs + 交叉工具链）
├── Busybox_1.30.0/           # BusyBox 源码（板子 init 体系）
├── Linux-4.9.88.tar.bz2      # 内核源码 tarball（本 VM 未解包，约 292MB）
└── Linux-4.9.88.code-workspace
```

### 2.2 交叉编译器（**唯一正确选择**）
- **编译器**：`arm-buildroot-linux-gnueabihf-gcc`（Buildroot 2020.02 自带，GCC **7.5.0**）
- **绝对路径**：
  ```
  /home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/bin/arm-buildroot-linux-gnueabihf-gcc
  /home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/bin/arm-buildroot-linux-gnueabihf-g++
  ```
- **sysroot**：
  ```
  /home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/arm-buildroot-linux-gnueabihf/sysroot
  ```
- **CMake 接入**：本项目通过 `cmake/user_cross_compile_setup.cmake` 设置上述变量
  （`CMAKE_C/CXX_COMPILER`、`CMAKE_SYSROOT`、pkg-config 指向交叉 sysroot 的 libevdev）。

> 📌 **为什么用 Buildroot 工具链而非 Linaro 6.2.1**：
> 1. LVGL v9 的 **evdev（触摸）后端依赖 `libevdev`**，Buildroot sysroot 已带 ARM 版；
>    Linaro 6.2.1 的 sysroot 没有，链接会报 `cannot find -levdev`。
> 2. 与板子 rootfs **同源**（同一 Buildroot 2020.02 / 同一 glibc），ABI 完全一致。
> 3. 若 sysroot 被清空（缺 `crt1.o` / `libc.so`），会报 `cannot find crt1.o`，无法链接。

### 2.3 宿主机工具（x86_64 侧）
| 工具 | 路径 | 版本 |
|------|------|------|
| cmake | `/usr/bin/cmake` | 3.22.1 |
| python3 | `/usr/bin/python3` | 3.10.12（LVGL Kconfig 生成需要，交叉编译务必传 `-DPython3_EXECUTABLE=/usr/bin/python3`） |
| dtc（设备树编译器） | `/usr/bin/dtc` | 1.6.1（Buildroot host 内也自带一份：`.../output/host/bin/dtc`） |

---

## 3. 设备树（Device Tree）— 重点

### 3.1 可编辑的设备树源（改 GPIO / I2C / 外设从这里改）
```
/home/book/DevelopmentEnvConf/imx6ullModule/dts/100ask_imx6ull-14x14.dts
```
同目录已编译产物：`/home/book/DevelopmentEnvConf/imx6ullModule/dts/100ask_imx6ull-14x14.dtb`

### 3.2 内核设备树源（如需从内核重新编译）
- 内核源码 tarball：`/home/book/100ask_imx6ull-sdk/Linux-4.9.88.tar.bz2`
- 解包后标准位置：`arch/arm/boot/dts/100ask_imx6ull-14x14.dts`
- ⚠️ **本 VM 当前未解包内核**，仅保留 tarball；需要内核态修改时先解包。

### 3.3 已编译 dtb 的分发位置
| 位置 | 用途 |
|------|------|
| `/home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/images/100ask_imx6ull-14x14.dtb` | SDK 构建镜像产出 |
| `/home/book/nfs_rootfs/100ask_imx6ull-14x14.dtb` | **板子 NFS 启动使用的 dtb** |
| `.../output/images/100ask_imx6ull_mini.dtb` | mini 版（不同载板） |

### 3.4 编译 / 反编译设备树
```bash
# 源 → dtb
dtc -I dts -O dtb -o out.dtb in.dts
# dtb → 源（逆向 / 核对）
dtc -I dtb -O dts -o out.dts in.dtb
```

---

## 4. 构建（Build）

### 4.1 PC 验证（SDL2 后端，默认）
```bash
cd ~/桌面/lv_port_linux
./build_pc.sh                                  # 增量构建（包装 cmake --build build_pc -j4）
# 首次 / defconfig 变更后需重建：
cmake -B build_pc -DLV_PORT_DEFCONFIG=configs/pc.defconfig \
      -DCMAKE_BUILD_TYPE=Debug .
cmake --build build_pc -j4
```
- 产物：`build_pc/bin/lvglsim`
- `configs/pc.defconfig`：SDL2 后端、`CONFIG_LV_COLOR_DEPTH_16=y`、`CONFIG_LV_DEF_REFR_PERIOD=16`

### 4.2 板子交叉编译（FBDEV + EVDEV）
```bash
cd ~/桌面/lv_port_linux
cmake -B build_board -DLV_PORT_DEFCONFIG=configs/board.defconfig \
      -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
      -DPython3_EXECUTABLE=/usr/bin/python3 .
cmake --build build_board -j$(nproc)
```
- `configs/board.defconfig`：FBDEV + EVDEV 后端、`CONFIG_LV_COLOR_DEPTH_32=y`、`CONFIG_LV_DEF_REFR_PERIOD=33`
- ⚠️ **PC 与板子 defconfig 差异只在后端三行**（SDL vs FBDEV/EVDEV），其余完全一致。

### 4.3 defconfig 机制
顶层 `CMakeLists.txt` 用 `LV_PORT_DEFCONFIG` 选择配置（默认 `configs/pc.defconfig`），
经 LVGL 的 Kconfig 体系（依赖 `Python3` 解释器）展开为 `.dotconfig`。

---

## 5. 真机运行环境（i.MX6ULL 开发板）

| 项 | 值 |
|----|----|
| 到达方式 | **ADB 唯一通道**（板子无以太网 IP、无 SSH），设备名 `100ask_IMX6ULL` |
| 显示设备 | `/dev/fb0`（权限 `root:video`，32bpp BGRX） |
| 触摸设备 | `/dev/input/event1`（Goodix，权限 `root:input`） |
| LVGL 环境变量 | `LV_LINUX_FBDEV_DEVICE`（默认 `/dev/fb0`）、`LV_LINUX_EVDEV_POINTER_DEVICE`（板子设 `/dev/input/event1`） |
| 自启脚本 | `/etc/init.d/S05lvgl`（BusyBox init，按 Sxx 顺序）→ `/usr/share/lvgl/lvglsim` |
| 重启 UI | `/etc/init.d/S05lvgl restart`（板子无 `pkill`，用 `killall` 或此脚本） |

- 抓屏（板子）：`dd if=/dev/fb0 of=/tmp/fb.raw bs=65536 count=40`（32bpp BGRX，1024×600×4 = **2457600** 字节）。
- 上板部署：本地二进制 → VM → 板子（`board_push`），日志落 `/var/log/`（板子 `/tmp` 是 tmpfs 约 250MB，勿堆日志）。

---

## 6. 网络服务（NFS / TFTP / QEMU）

| 服务 | 路径 / 说明 |
|------|-------------|
| NFS 导出 | `/home/book/`（导出选项 `rw,no_root_squash,insecure,async`）→ 板子根文件系统 |
| TFTP | `/home/book/tftpboot`（内核 / dtb 网络加载） |
| QEMU 虚拟板 | `/home/book/ubuntu-18.04_imx6ul_qemu_system`（不开硬件即可启动 `mcimx6ul-evk`） |
| NFS 启动 dtb | `/home/book/nfs_rootfs/100ask_imx6ull-14x14.dtb` |

---

## 7. 关键路径速查表（Agent 直接 grep 用）

| 类别 | 路径 |
|------|------|
| 项目根 | `/home/book/桌面/lv_port_linux` |
| 交叉编译器 | `/home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/bin/arm-buildroot-linux-gnueabihf-gcc` |
| 交叉 sysroot | `/home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/arm-buildroot-linux-gnueabihf/sysroot` |
| 交叉编译 CMake | `~/桌面/lv_port_linux/cmake/user_cross_compile_setup.cmake` |
| 设备树源 | `/home/book/DevelopmentEnvConf/imx6ullModule/dts/100ask_imx6ull-14x14.dts` |
| 设备树产物（SDK） | `/home/book/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/images/100ask_imx6ull-14x14.dtb` |
| 设备树产物（NFS） | `/home/book/nfs_rootfs/100ask_imx6ull-14x14.dtb` |
| 内核 tarball | `/home/book/100ask_imx6ull-sdk/Linux-4.9.88.tar.bz2` |
| PC 构建产物 | `~/桌面/lv_port_linux/build_pc/bin/lvglsim` |
| FBDEV 后端代码 | `~/桌面/lv_port_linux/ports/lv_port/display_backends/fbdev.c` |
| EVDEV 后端代码 | `~/桌面/lv_port_linux/ports/lv_port/indev_backends/evdev.c` |
| 板子 UI 自启 | `/etc/init.d/S05lvgl`（板子，ADB 通道） |

---

## 8. 常见坑（Agent 注意）

1. **root 不可用**：本 VM 不要 `sudo` / `su root`；需 root 的板子操作走 ADB（`as_root=True`，内部 base64 管道 + `su`）。
2. **设备树改完要重新编译 dtb** 并替换到 `nfs_rootfs/` 或 TFTP 目录，板子才会生效。
3. **交叉编译务必带 `-DPython3_EXECUTABLE=/usr/bin/python3`**，否则 Kconfig 生成阶段找不到 python。
4. **路径一律用绝对路径**：SSH API 的 `read_file/pull_file` 不展开 `~`；bash 双引号内 `~` 也不展开。
5. **不要同时开两台同名 IP 的 VM**，会抢 192.168.150.139。
6. **USB 串口（CH340 / FM225）被 brltty 抢占**时会丢 `/dev/ttyUSB0`：先 `systemctl mask brltty-udev.service` + 重载 `ch341`。
