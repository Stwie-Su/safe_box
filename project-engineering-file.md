# 项目工程档案：safe_box（i.MX6ULL + LVGL 智能保险柜）

> 最后更新：2026-10-19　当前分支 `main` @ `d1376b2`
> 本文件记录「为什么这么做」与「现在做到哪」，需求细节仍以 `规约/需求规约.md` 为准。

## 1. 项目概况

一句话：在 100ask i.MX6ULL 开发板上运行的多用户智能保险柜（LVGL v9 界面 + PIN/人脸/远程开锁）。

当前阶段：**⑤ 部署运维（生产化补齐）**。应用功能与 28 项自动化测试已闭环；
守护链路（自启 / 崩溃重启 / 看门狗 / 配置校验）代码与 PC 验证完成，**板上真机验证待做**（板子未接入）。

## 2. 技术栈与版本

| 项 | 版本 / 说明 | 来源 |
|---|---|---|
| SoC | NXP i.MX6ULL，Cortex-A7 单核，512MB | 硬件 |
| 系统 | Buildroot 2020.02.x（BusyBox init，无 systemd） | SDK 目录 |
| 交叉编译器 | `arm-buildroot-linux-gnueabihf-gcc` 7.5.0 | `Buildroot_2020.02.x/output/host/bin` |
| 构建 | CMake ≥ 3.16；PC 走 SDL2，板端走 FBDEV+EVDEV | `CMakeLists.txt` |
| 界面 | LVGL v9（vendored 在 `lvgl/`） | 仓库 |
| 远程 | 自研 MQTT 3.1.1 + mbedTLS（可选） | `app/core/remote/` |
| 存储 | JSON 文件（`users.json` / `network.json` / `safe.log`），原子写 | `app/core/store/` |
| 测试 | ctest 28 项（PC），含 8 组 supervisor 进程级用例 | `tests/` |

## 3. 架构决策记录（ADR）

| 编号 | 场景 | 决策 | 理由 | 代价 | 状态 |
|---|---|---|---|---|---|
| ADR-001 | 应用崩溃后无人拉起 | 独立 supervisor 进程（fork/exec + waitpid），不在应用内自重启 | 崩溃时应用自身状态不可信；父进程只依赖内核语义 | 多一个进程要维护 | 已落地 |
| ADR-002 | 应用活着但卡死 | 主循环心跳喂 `/dev/watchdog`，卡死即停喂 | 只有主循环知道业务是否真的在跑；独立线程喂狗会让看门狗形同虚设 | PC 无狗，只能走 null 后端 | 已落地，板上待验 |
| ADR-003 | 反复崩溃后进安全态，没人喂狗 → 整机复位循环 | 安全态由 supervisor **接管看门狗**持续喂狗，保持锁闭等人修 | 失效要导向安全侧且有痕迹；整机反复复位会丢现场 | supervisor 要理解 watchdog ioctl | 已落地，板上待验 |
| ADR-004 | 配置错误静默进入运行期 | 启动时集中校验，分三级：FATAL 不可降级 / FATAL 可降级 / WARN | 配错应在启动时暴露；但阈值越界不该让设备起不来 | 多一个模块与一套测试 | 已落地 |
| ADR-005 | 重复启动会抢应用与看门狗 | `flock` 单实例 + `--daemon` 双 fork + pidfile | init 脚本可能被手工重复执行；两个 supervisor 会互相 kill | 需要可写的 `/var/run` | 已落地 |
| ADR-006 | `--daemon` 后 stdio 指向 /dev/null，应用日志丢失 | supervisor `--app-log` 把子进程 stdout/stderr 接到文件；应用侧行缓冲 | 日志是排障第一现场；外部 `stdbuf` 在 Buildroot 不一定有 | 多一个参数 | 已落地 |

## 4. 模块清单与进度

| 模块 | 状态 | 说明 |
|---|---|---|
| `app/core/config_validate.*` | 已完成 | 纯函数校验 + 分级 + 钳制；`tests/test_config_validate.c` 覆盖 |
| `app/core/config.c` | 已完成 | `app_config_init()` 末尾接入校验，FATAL 返回错误 |
| `app/main.c` / `app/app.c` | 已完成 | `app_main()` 返回 int；启动中止时停狗（magic close）后退出 2 |
| `tools/supervisor/safe_supervisor.c` | 已完成 | daemon / flock / status / 日志轮转 / 退避 / 安全态接管狗 |
| `tools/supervisor/run_with_supervisor.sh` | 已完成 | 一次性环境准备 + `exec supervisor --daemon` |
| `deploy/S05safe` | 已完成（未上板） | BusyBox init 的 start/stop/restart/status |
| `deploy/safe_state.sh` | 已完成（GPIO 待接） | 安全态留痕；真锁接入后补 GPIO 拉低 |
| `deploy/safe_supervisor.service` | 模板 | systemd 备用；未实测 |
| `README.md` | 已完成 | 新增「生产运行」：进程模型 / 文件位置 / 命令 / 退出码 / 配置校验 / 排障表 / 上板清单 |
| `tests/test_supervisor.sh` | 已完成 | 8 组进程级用例，随 ctest 跑（cc 缺失时 skip） |

## 5. 接口约定

### 5.1 配置校验（`app/core/config_validate.h`）

```c
void app_config_validate(const app_config_t *cfg, cfg_report_t *out); /* 纯函数 */
void app_config_report(const cfg_report_t *r);
int  app_config_clamp(app_config_t *cfg);   /* 返回被改字段数 */
bool app_config_strict(void);               /* SAFE_CONFIG_STRICT，缺省 true */
```

`cfg_report_t`：`total` / `count` / `fatal_count` / `fatal_blocking` / `warn_count`。
`fatal_blocking > 0` ⇒ 即使非严格模式也拒绝启动（数据目录这类）。

### 5.2 supervisor 命令行

```
--app PATH --max-restarts N --window SEC --grace SEC --restart-delay MS
--safe-cmd CMD --watchdog DEV|none --log FILE --log-max BYTES
--app-log FILE --status FILE --pidfile FILE --daemon
-- 之后全部原样传给应用
```

退出码：`0` 正常退出 / `1` 参数或启动错误 / `2` 安全态 / `3` 已有实例。

### 5.3 状态文件（`--status`，原子写）

```
pid=<pid>
state=running|safe|stopped
restarts=<累计重启次数>
since=<epoch 秒>
last_exit=exit:<code> | signal:<NAME>(<num>) | running | safe-state | supervisor-exit
```

## 6. 已知风险

| 风险 | 影响 | 现状 / 缓解 |
|---|---|---|
| 板子未接入 | 真狗、安全态、S05 均未实测 | 先按 README「上板自检清单」逐步验证；未验证前不要把 S05 切生产 |
| `/dev/watchdog` 驱动差异 | `WDIOC_SETTIMEOUT` 可能被向上取整或拒绝 | 代码已容忍失败（沿用驱动默认），需在目标板 `dmesg` 确认实际超时 |
| 安全态接管狗未实测 | 若驱动不允许二次 open，设备仍会复位 | `watchdog_takeover` 失败会明确记日志；这是无狗可接管时的最后手段 |
| `safe_state.sh` 的 GPIO 未接 | 安全态只留痕、未真正保持锁闭 | 真锁接入后按注释补 sysfs 写值，并在板上 `echo` 验证引脚编号 |
| systemd 模板未实测 | 参数组合可能与 systemd 生命周期冲突 | 已写 `RestartPreventExitStatus=2 3` 且不加 `--daemon`；上 systemd 板子前先手工跑 |
| 日志轮转只保留一份历史 | 长时间崩溃循环会覆盖更早现场 | 崩溃循环本身会进安全态，日志量有限；需要长期留档时接 syslog |
| 配置校验只覆盖 `app_config_t` | `pin_min_len/pin_max_len` 由 store 自行钳制 | store 已有钳制逻辑；两处范围需同步维护 |

## 7. 待办

1. 上板自检 6 步（README）：supervisor 手工跑 → 崩溃重启 → 安全态 → `/dev/watchdog` → `kill -STOP` 复位 → S05 断电重上电。
2. S05 接线：确认板上现网 `S05lvgl` 的调用方式，替换为 `S05safe`（保留回滚脚本）。
3. 真锁 GPIO：`safe_state.sh` 补拉低动作；`hal/actuator` 接 GPIO 后端。
4. 72h 长跑：看门狗不误触发、fd/内存无增长、日志轮转生效。
5. 若上 systemd 板子：实测 unit 文件，补 `WatchdogSec` 与 `sd_notify`（可选）。
