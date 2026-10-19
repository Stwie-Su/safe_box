# safe_box — 基于 i.MX6ULL + LVGL 的嵌入式智能保险柜

一个面向嵌入式 Linux 应用开发岗位的演示项目：在百问网 i.MX6ULL 开发板上，用 LVGL v9
实现一个**纯离线、局域网**运行的智能保险柜人机界面与核心逻辑。项目覆盖多进程/多线程与
IPC、串口二进制协议状态机、socket 网络编程、掉电安全持久化、守护进程/定时器/信号、
交叉编译与 Buildroot 等嵌入式岗核心考点。

## 硬件平台

- SoC：NXP i.MX6ULL（ARM Cortex-A7）
- 显示：1024x600 RGB565，电容触摸屏
- 人脸识别模组：FM225（海凌科 HLK，UART 串口协议，识别在模组端完成）
- 构建：Buildroot 交叉编译 + PC（SDL2）验证优先

## 功能

- 三角色用户模型（主管理员 / 普通用户 / 临时用户-限量限时）
- 认证：4–8 位 PIN（PBKDF2 加盐哈希）+ 人脸凭据；`auth_method` 预留 OTP
- 掉电安全配置持久化（JSON + 抽象存储层，无独立 rootfs）
- 审计日志独立顶层入口（记录 PIN 哈希与操作，不落地明文口令）
- 远程指令权限分级：自研 MQTT 3.1.1 客户端（协议编解码 / 在途队列 / 状态机，mbedTLS 双向 TLS）+ rpc_guard 凭据鉴权与去重
- 五套可切换主题（石墨黑 / 月白 / 蓝白 / 松石青 / 浅蓝，默认浅蓝）
- 相机预览（UVC）、模组健康与降级、启动对账

## 构建

```bash
# PC 验证（SDL2 后端）
./build_pc.sh

# 交叉编译（板子，完整命令与环境变量见 CLAUDE.md §5）
cmake -B build_board -DLV_PORT_DEFCONFIG=configs/board.defconfig \
  -DCMAKE_TOOLCHAIN_FILE=cmake/user_cross_compile_setup.cmake .
cmake --build build_board -j$(nproc)
```

## 生产运行（上板部署）

> 这套流程已在 PC（Ubuntu 22.04）上跑通；板上首次部署请按最后一节「上板自检清单」
> 逐项确认，不要一次性替换掉现网还能用的启动脚本。

### 进程模型

```
BusyBox init
  └─ /etc/init.d/S05safe start
       └─ safe_boot.sh                  一次性：fb/触摸/串口/RTC、broker 扫描、OTA 标记
            └─ safe_supervisor --daemon 常驻：崩溃重启、限速、安全态、看门狗接管
                 └─ lvglsim            应用：主循环喂狗，卡死即停喂 → 硬件复位
```

两层保护各管一段，别混：

| 层 | 管什么 | 不管什么 |
|---|---|---|
| supervisor | 进程崩溃、非 0 退出、反复崩溃后进安全态 | 进程还活着但卡死 |
| 硬件看门狗 | 主循环卡死、死锁（停喂 → 复位整机） | 崩溃后由谁拉起（那是 supervisor） |

### 没有硬件看门狗时（本项目的实际情况）

板子上没有 `/dev/watchdog`，所以看门狗能力按三档降级：

| 后端 | 能做什么 | 不能做什么 |
|---|---|---|
| `linux`（真狗） | 主循环停喂 → 超时复位整机 | 需要芯片/内核支持 |
| `file`（软狗） | 心跳停 → supervisor 判定卡死 → 杀掉并重启应用 | 内核 panic、整机死机、supervisor 自己挂掉、断电 |
| `null`（占位） | 保住调用链，便于 PC 跑同一份代码 | 没有任何检测能力 |

选择规则：不指定 `SAFE_WATCHDOG_BACKEND` 时自动按 **linux → file → null** 降级。
所以现在板子走 `file`；以后接了真狗或打开内核 WDOG，同一份代码会自动优先用真狗，不用改配置。

软狗怎么工作：

```
应用主循环每拍 hal_watchdog_kick()（默认 1s 节流）
  → 把单调时钟毫秒值写进 /var/run/safe_heartbeat
supervisor 每秒读这个值，与自己的单调时钟比
  ├─ 超过 --heartbeat-timeout（默认 5s）没推进 → SIGTERM → 宽限后 SIGKILL → 按异常重启
  ├─ 内容是 stopped → 应用正常退出，不重启
  ├─ 文件不存在（真狗场景 / 还没启动起来）→ 只提示，不判死
  └─ 启动宽限 --heartbeat-startup（默认 30s）内即使过期也再等等，避免初始化慢被误杀
```

★ 为什么写单调时钟而不是文件修改时间：这台设备有 DS3231 校时，还有一条远程 `sync_time`
能把系统时钟拨到任意点。用墙上时钟判定的话，把时钟往未来一拨，心跳就"永远不过期"，
软狗被一条远程指令绕过。单调时钟不受校时影响，两边同一基准，比较才有效。

**能力边界要说清楚**：软狗只能覆盖「进程活着但主循环卡死」，覆盖不了内核死机与整机断电。
要补上那一层只有两条路：开 i.MX6ULL 内置 WDOG（内核 `CONFIG_IMX2_WDT` + 设备树使能），
或加一颗外置看门狗芯片。先确认板子到底有没有：

```sh
ls /dev/watchdog*                        # 有没有设备节点
zcat /proc/config.gz | grep -i IMX2_WDT  # 内核有没有编 SoC 看门狗驱动
dmesg | grep -i wdog                     # 驱动有没有起来
```

### 文件放哪

| 内容 | 路径 |
|---|---|
| 应用 / supervisor / 启动脚本 | `/usr/share/lvgl/`（`lvglsim`、`safe_supervisor`、`safe_boot.sh`） |
| 开机脚本 | `/etc/init.d/S05safe` |
| 安全态动作 | `/etc/safe/safe_state.sh` |
| 用户表 / 审计日志 | `/var/lib/safe/`（`users.json`、`network.json`、`safe.log`） |
| 应用日志 | `/var/log/lvglsim.log` |
| supervisor 日志 | `/var/log/safe_supervisor.log`（超过 1MB 轮转为 `.1`） |
| 运行状态 | `/var/run/safe_supervisor.status`、`/var/run/safe_supervisor.pid` |

仓库里对应 `deploy/S05safe`、`deploy/safe_state.sh`、`deploy/safe_supervisor.service`、
`tools/supervisor/run_with_supervisor.sh`（安装到板子后改名为 `safe_boot.sh`）。

### 常用命令

```sh
/etc/init.d/S05safe start      # 启动（supervisor 自己转后台）
/etc/init.d/S05safe status     # 看状态文件 + 进程是否活着
/etc/init.d/S05safe stop       # 先 TERM 让应用优雅退出，10s 不退才 KILL
/etc/init.d/S05safe restart

cat /var/run/safe_supervisor.status   # pid / state / restarts / last_exit
tail -f /var/log/safe_supervisor.log  # 重启与安全态记录
tail -f /var/log/lvglsim.log          # 应用日志（行缓冲，崩溃不丢最后一行）
```

支持 systemd 的系统用 `deploy/safe_supervisor.service`。两处关键差异写在单元文件注释里：
不要加 `--daemon`（systemd 自己管后台），并且要 `RestartPreventExitStatus=2 3`
（安全态与「已有实例」不能触发 systemd 重启，否则安全态会被绕过）。

### 退出码

| 退出码 | 含义 | 谁该处理 |
|---|---|---|
| 0 | 应用正常退出，未重启 | 无需处理 |
| 1 | 参数错误 / 应用不存在或不可执行 | 部署脚本写错，修脚本 |
| 2 | 反复崩溃，已进入安全态（保持锁闭，看门狗由 supervisor 接管） | 人工介入：看日志、修应用、重启 |
| 3 | 已有 supervisor 在跑（单实例锁） | 正常现象，不要重复启动 |

### 配置校验（启动第一道闸）

启动时对配置做一次检查，分三级处理：

| 级别 | 例子 | 处理 |
|---|---|---|
| FATAL（不可降级） | 数据目录不存在 / 不可写 | **拒绝启动**，退出码 2 |
| FATAL（可降级） | 启用远程但端口非法、要求 TLS 却没有 CA | 严格模式拒绝启动；`SAFE_CONFIG_STRICT=0` 时关闭远程通道后继续（不会退化成明文） |
| WARN | 阈值越界、日志条数超限、人脸后端名写错 | 钳制到合法范围并打印，继续启动 |

默认严格模式。排查时可临时 `SAFE_CONFIG_STRICT=0`，但它只对「可降级」项有效——
数据目录坏了设什么都不放行。日志直接 grep `[CONFIG]`：

```
[CONFIG] 校验发现 1 项问题（FATAL 1 [其中不可降级 1] / WARN 0）
[CONFIG] FATAL data_dir：目录存在且可写（不可降级）
[CONFIG] 拒绝启动：1 项 FATAL（其中不可降级 1）
```

### 出问题怎么查

| 现象 | 先看什么 | 常见原因 | 处理 |
|---|---|---|---|
| 开机没界面，进程不在 | `/var/log/safe_supervisor.log` 有没有「supervisor 启动」 | 脚本没执行权限、`safe_boot.sh` 路径不对 | `ls -l` 查权限，手工跑一次看报错 |
| 起来就退出，退出码 2 | `/var/log/lvglsim.log` 里的 `[CONFIG]` | 配置越界、数据目录不可写、TLS 缺 CA | 按日志改配置；目录 `mkdir -p` + `chmod` |
| 反复重启 | `status` 里的 `last_exit` 与日志中的「异常退出」 | 应用崩溃（段错误/断言）、依赖文件缺失 | 拿 `last_exit=signal:SIGSEGV` 去查 core 与日志最后几行 |
| 进了安全态（state=safe） | `/var/log/safe_state.log`、应用日志最后 20 行 | 60s 内崩溃超过 5 次 | 修好根因后 `S05safe restart` 人工恢复 |
| 整机隔十几秒复位一次 | 串口内核日志有没有 watchdog reset | 应用卡死停喂狗，或安全态下 supervisor 没接管 | 确认主循环没死锁；看 supervisor 是否打印「已接管看门狗」 |
| 应用被反复重启，日志有「心跳超时」 | `status` 的 `last_exit=heartbeat-timeout` | 主循环死锁 / 长时间阻塞（软狗判定） | 查应用日志最后几行；软狗不覆盖内核死机 |
| 关机变成重启 | `[WATCHDOG] magic close` 相关日志 | stop 用了 KILL，没走优雅退出 | 用 `S05safe stop`，不要 `kill -9` |
| 远程连不上 | `[CONFIG]` 的 TLS 项、`SAFE_MQTT_HOST` | CA/证书路径错、端口错、broker 地址漂了 | 先手工跑一次 `find_broker.py` |
| 日志不增长 | `/var/log` 是否可写、`--app-log` 路径 | 分区满 / 权限不对 / 忘了 `--app-log` | `df -h`、`touch` 试写 |

### 上板自检清单

按顺序做，每步看到预期输出再进下一步：

1. `cc -O2 -Wall -Wextra -o safe_supervisor safe_supervisor.c` 能编过（交叉编译换成板端工具链）。
2. `./safe_supervisor --app ./lvglsim --watchdog none --status /tmp/s.status --pidfile /tmp/s.pid`
   能起来，`kill -TERM` 后退出码 0。
3. 故意让应用退出 1，确认 supervisor 会重启，并在超过上限后打印 `SAFE-STATE`。
4. `ls /dev/watchdog`：有则确认日志是 `后端=linux`（真狗）；没有则确认是 `后端=file`（软狗）。
5. 软狗验证：对 `lvglsim` 执行 `kill -STOP <pid>`，等 `--heartbeat-timeout`（默认 5s），
   确认 supervisor 日志出现「心跳超时」并重启应用。要验真狗复位再单独做（**会重启设备，最后再做**）。
6. 换成 `S05safe` 自启，断电重上电，确认界面起来且 `status` 里 `state=running`。

## FM225 人脸识别模组

FM225 的识别在模组端完成，板子通过 UART 收发指令帧。本项目 `app/hal/face/` 下的协议
状态机与 `tools/` 下的自检/模拟脚本为**自行逆向的成果**。海凌科官方手册与官方调试工具为
厂商私有资料，**未包含在本仓库**，请向厂商获取（本地开发环境置于 `资料/`，该目录不入库）。

## 许可证

- 本项目（仓库根目录）：MIT License —— 见 [LICENSE](LICENSE)
- `lvgl/` 为 vendored 的 LVGL v9，沿用其自身的 MIT License（Littlev Graphics Library）

## 工程目录结构

```
lv_port_linux/
├─ app/            产品源码（分层：ui / core / hal / platform）
│  ├─ ui/          页面与组件（pages/ 走 file(GLOB)，新增文件自动纳入构建）
│  ├─ core/        业务核心（auth / store / remote / support）
│  ├─ hal/         硬件抽象（camera / face / net / time / storage / actuator）
│  └─ platform/    PC(SDL) 与板端(FBDEV) 双平台后端
├─ configs/        Kconfig 双平台 defconfig（pc / board）
├─ cmake/          工具链与平台 CMake 片段
├─ deploy/         上板部署件：S05safe / safe_state.sh / systemd 单元模板
├─ tests/          ctest 用例（纯逻辑 + 进程级行为，可确定性验证）
├─ tools/          构建/部署/排查脚本（含 OTA、证书、抓屏、服务发现）
├─ 规约/           需求规约 / 技术路线规约 / Bug案例集 + 归档/（全量 Bug 台账）
├─ lvgl/           LVGL 子模块（63M，**不要 grep**）
├─ ports/          LVGL 官方移植层头文件（platform/CMakeLists 引用）
├─ third_party/    第三方源码：stb_image / aes / sha256（camera 与 crypto 使用）
├─ build_pc/       PC 构建产物（gitignore）
└─ build_board/    交叉构建产物（gitignore）
```

**构建**：`./build_pc.sh`（PC/SDL）；
交叉构建 `cmake -B build_board --toolchain cmake/arm-linux.cmake -DPython3_EXECUTABLE=/usr/bin/python3 && cmake --build build_board -j4`。
**测试**：`ctest --test-dir build_pc`（29 项，含进程级 supervisor 行为测试与软狗测试）。
**文档入口**：面向人看 README；面向 agent 看 `CLAUDE.md`（含目录禁区与省上下文纪律）；
需求/设计看 `规约/需求规约.md` + `规约/技术路线规约.md`；排错先看 `规约/Bug案例集.md`（定位方法论），
缺陷清单查 `规约/归档/Bug台账_全量.md`。

厂商资料（PDF）不入库，已移至 `~/桌面/中间产物/06_资料/`。
