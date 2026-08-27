# LVGL v9 双平台配置对比：Ubuntu(PC) vs 开发板（不用 .config）

> 工程：`/home/book/Desktop/lv_port_linux`
> 改动原则：**彻底不用 `.config`**，PC 与开发板各用一个 `defconfig`，差异**只在后端三行**，业务代码零改动。
> 切平台 = 换 `defconfig` + 换编译器，源码不动。

---

## 1. 配置差异（defconfig 层）

两套文件：**`configs/pc.defconfig`**（默认，PC）与 **`configs/get_started.defconfig`**（开发板）。
除下表“后端”三行外，其余配置（标准库/OS、颜色深度、刷新周期、日志、字体、关闭 examples/demos）**完全相同**。

| 配置项 | Ubuntu / PC（`pc.defconfig`） | 开发板（`get_started.defconfig`） | 含义 |
|--------|------------------------------|----------------------------------|------|
| `CONFIG_LV_USE_SDL` | `=y` | `is not set`（注释） | PC：SDL2 桌面窗口 |
| `CONFIG_LV_USE_LINUX_FBDEV` | `is not set`（注释） | `=y` | 板子：直接写 `/dev/fb0` |
| `CONFIG_LV_USE_EVDEV` | `is not set`（注释） | `=y` | 板子：读 `/dev/input` 触摸屏 |
| `CONFIG_LV_USE_LINUX_DRM` | `is not set` | `is not set` | 两者都关 |
| 其余（CLIB/OS/COLOR/LOG/字体…） | 相同 | 相同 | — |

> 两套文件里都用注释把“对方平台的那几行”写出来并标注 `← 开发板/PC 这行改成 =y`，
> 所以**一个文件就能看全两边的差异**，无需对照两个文件。

---

## 2. 代码差异（main.c 层）

`main.c` 不做平台分支拷贝，而是用 **预处理宏自动选择**，效果等价于“板子实现暂被注释”：

```c
/* 显示后端初始化：Ubuntu(PC) vs 开发板 对照
 * 后端由 defconfig 决定编译哪些 .c（pc->sdl.c；board->fbdev.c）。
 * PC 上 LV_USE_SDL 生效走 SDL 分支；板子上 LV_USE_LINUX_FBDEV 生效走 FBDEV 分支
 * （SDL 分支被编译剔除 = 等效“注释掉”）。上板只需切 defconfig，本文件不动。 */
if(selected_backend != NULL) {                 /* ./lvglsim -b SDL | -b FBDEV 可强制指定 */
    if(driver_backends_init_backend(selected_backend) == -1) die(...);
}
#if LV_USE_SDL                                   /* --- PC：SDL2 桌面窗口（当前生效）--- */
    else if(driver_backends_init_backend("SDL") == -1) die("...SDL...");
#elif LV_USE_LINUX_FBDEV                         /* --- 板子：FBDEV 写 /dev/fb0（上板自动生效）--- */
    else if(driver_backends_init_backend("FBDEV") == -1) die("...FBDEV...");
#endif
else if(driver_backends_init_backend(NULL) == -1) die(...);   /* 兜底默认后端 */
```

EVDEV 同理，已存在 `#if LV_USE_EVDEV` 块（板子生效、PC 自动剔除）。

> 为何用 `#if/#elif` 而非手写 `/* */` 注释：手写注释在切到板子 defconfig 时不会自动“复活”，
> 会导致板子没有显示初始化、编译出的二进制跑不起来；`#if` 方式切 defconfig 即自动启用，零手动操作。
> 若你只想在代码里“看到”板子写法，defconfig 文件里已经把对方平台那几行用注释列出来了。

---

## 3. 切换平台（构建命令）

CMake 通过 `LV_PORT_DEFCONFIG` 选择配置（默认 `configs/pc.defconfig`）。

### PC（Ubuntu 桌面，看真实 UI 窗口）
```bash
cd ~/Desktop/lv_port_linux
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig        # 指向 SDL2 2.28.5
CMAKE=~/tools/cmake-3.22.1-linux-x86_64/bin/cmake
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
$CMAKE -B build_pc -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build_pc -j$(nproc)
DISPLAY=:0 ./build_pc/bin/lvglsim        # 进 VM 桌面才看得到窗口
```

### 开发板（i.MX6ULL，交叉编译）
```bash
cd ~/Desktop/lv_port_linux
BR=~/100ask_imx6ull-sdk/Buildroot_2020.02.x
export PATH=~/tools/cmake-3.22.1-linux-x86_64/bin:$BR/output/host/bin:$PATH
# 关键：指定板子 defconfig + 交叉工具链文件
cmake -B build -DLV_PORT_DEFCONFIG=configs/get_started.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake \
  -DPython3_EXECUTABLE=$BR/output/host/bin/python3 .
cmake --build build -j$(nproc)            # 产物 build/bin/lvglsim（ARM ELF）
# 拷到板子根文件系统直接运行（NFS 挂载或 scp）
```

---

## 4. 已验证

- ✅ PC：`pc.defconfig` 配置+原生编译通过；`DISPLAY=:0 ./lvglsim` 实跑启动正常（`[ACTUATOR] PC mock mode ... init OK`）。
- ✅ 板子：`get_started.defconfig` + Buildroot 交叉工具链配置+编译通过；产物为 ARM ELF（动态链接 `ld-linux-armhf.so.3`）。
- ⚠️ 板子配置**不能**用原生 x86 gcc 编译：`evdev` 后端依赖 `libevdev`，仅存在于 Buildroot 交叉 sysroot，主机无此库（报 `libevdev not found`）。这是预期行为，用交叉工具链即可。

## 5. 已知限制（沿用）
- VM apt 镜像 DNS 失效，`libsdl2-dev`/`build-essential`/`libx11-dev` 等处于 `iU` 破损态——不影响 native/交叉编译（文件都在盘上）；彻底修需恢复网络后 `sudo apt --fix-broken install`。
- 看 PC 画面必须进 VM 桌面；纯 SSH 会话 `DISPLAY` 为空，需显式 `DISPLAY=:0` 或 Windows 装 VcXsrv 用 `ssh -X`。
