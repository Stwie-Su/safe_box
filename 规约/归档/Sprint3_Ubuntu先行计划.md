# Sprint3 · Ubuntu 先行开发计划

> 版本：v1.0　制定日期：2026-09-08
> 依据：《技术路线规约》v1.3 §3.4（face 线程）/ §4.2（先 Ubuntu 验证再上板）/ §10（重构路线）；《需求规约》v1.6 FR-16（预览）/ FR-19（按原因分流）
> 执行环境：Ubuntu VM `192.168.150.139`，PC/SDL 构建，**本期不上板**
> 配套资料：`~/桌面/资料/FM22x系列人脸锁算法模组用户开发手册V1.7.pdf`（协议唯一依据）
> 进度联动：**每完成一步，必须更新《开发进度.md》§4 当前步指针**，并回填本文件对应步骤的 ✅ 与实测数据

---

## 0. 目标与边界

目标：Sprint3 除「真模组联调 / 上板」外的全部工作在 Ubuntu 完成，做到 **FM225 到货只差接线**。

| 现在做（本计划） | 等硬件（不在本计划内） |
|---|---|
| camera_v4l2 后端 + 真实预览；face 线程；FM225 协议状态机（socat 虚拟串口验证）；性能验收；R1–R3 重构止血 | 真模组联调、活体实测、I2C RTC、FBDEV / Buildroot 上板、执行器 GPIO |

数据流（规约 §3.4）：

```
USB 摄像头(UVC) ─┐
                 ├─ face 线程 poll() 双 fd ─┬─ 预览帧 → page_face (320×240)
socat 虚拟串口 ──┘   （只转最新帧/协议状态机）└─ 识别结果 → event_bus_post(EV_FACE_EVENT) → auth_fsm
真 FM225 到货后替换 socat 这一格，其余零改动
```

## 0.1 前置检查（动手前一次性完成，结果记入《开发进度.md》）

```bash
lsusb                                          # 摄像头已挂入 VM（VMware → 可移动设备 → 连接）
sudo apt install v4l-utils
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext     # 确认 YUYV/MJPEG 与 320x240@fps 能力
```

- 支持 **YUYV @ 320×240 ≥20fps** → 按规约 1:1 直通（最理想）；
- 仅 640×480 → 整数倍降采样（规约 §4.2 调整规则）；
- 仅 MJPEG → 需 DCT 解码，代价大，**停下回报用户再定**（规约允许协商）。

---

## 1. 任务分解（严格按序执行，不得跳步）

### 步骤 0：重构止血 R1–R3（半天）　【✅ 已完成 · 2026-09-08】

| 编号 | 内容 | 代码位置 | 验收 |
|---|---|---|---|
| R1 | `write_file_all` 改 **tmp → `fsync` → `rename` → `fsync`(目录)** 原子替换（users.json / network.json 共用） | `app/core/store/store.c` | ①新增 kill -9 崩溃一致性自测脚本：循环写盘期间任意时刻 kill -9，重启后 users.json 可解析且要么旧版要么新版；②`ctest` 全绿 |
| R2 | 三处 `time(NULL)` → `hal_time()`（同时排查全工程其余 time(NULL)，日志时间戳一并走 hal_time） | `app/core/auth/unlock_backend.c`（约行 29/146/161）等 | `grep -rn 'time(NULL)' app/` 仅允许出现在 `hal/time/time_sys.c` 内部 |
| R3 | 删除 `credentials.c/h` + `app/core/CMakeLists.txt` 对应编译条目 | `app/core/store/` | `grep -rn credentials_ app/` 零引用；构建通过 |

> 顺带（同一提交）：把《开发进度.md》§3 列的未提交 UI/动态码改动一并整理 commit，保持主线干净。
>
> **实测结果（2026-09-08，PC/SDL 构建）**
>
> | 项 | 结果 |
> |---|---|
> | 提交 | `17eba52`（整理未提交改动）、`7c4e7f2`（R1）、`9dddd95`（R2）、`fb219da`（R3） |
> | 构建 | `rm -rf build_pc` 全量重建 0 error；告警全部为基线既有（LVGL deprecated API / 未用变量 / `platform.h` `-Wcomment`），**无新增告警** |
> | ctest | **5/5 通过**（test_totp / test_auth_fsm / test_store / **test_store_crash** / check_layers），2.90s |
> | R1 验收① | 原子版：盯梢 10 轮 ×100ms，累计采样 **708,574 次，中间态 0 次**；随机轮 10 轮全过。反向对照（临时换回非原子 `write_file_all`）：盯梢 **10/10 FAIL**，抓到 50 次中间态（size=0 / 4096 / 24576…），终态文件截断到只剩 0~9 条用户——用例确有牙齿 |
> | R2 验收 | `grep -rn "time(NULL)" app/` 只剩 `app/hal/time/time_sys.c:15`（`platform_fbdev.c` 那条是 LVGL API `lv_display_get_inactive_time(NULL)` 的子串误报，用词边界 `\btime(NULL)` 即只剩 time_sys.c） |
> | R3 验收 | `grep -rn "credentials" app/ **零引用**；删除后全量重建通过 |
>
> 崩溃自测位置：`tests/test_store_crash.c`（已注册进 ctest，默认 10 轮）+ `tools/crash_consistency_test.sh [轮次]`（默认 50 轮长跑）。
> 设计说明：单纯「随机时刻 kill -9」在本机几乎撞不到截断窗口（glibc 一次大块 fwrite 只发一次 `write()`，而 Linux 不会腰斩已进入内核的 `write()`，实测旧实现也能连过 60 轮），故增加盯梢轮——父进程连续 `stat()` 目标文件，全程断言大小恒为完整值。

### 步骤 1：face_reason_t 接口切换（R8 的 face 部分，1 天）　【✅ 已完成 · 2026-09-08】

规约 §5.2 / §5.5 / §5.11 的**目标接口**在本步落地（commit `8963f6d` 主体 + `29b7a82` 收尾）：

1. `hal_face.h`：`face_result_t` 删 `score`，按 §5.11 增 `face_reason_t`（OK / NO_MATCH / LIVENESS_FAIL / TIMEOUT / ERROR）；`face_service_inject(id, reason)`；新增 `face_reason_name()` 供日志/RPC。
2. `auth_fsm`：`submit_detect(id,score)` → `submit_face(id, reason)`；`resolve()` 改按原因分流——OK→开锁；NO_MATCH→计数，连续达 `face_otp_after` 转 WAIT_OTP，再达 `max_failed` LOCKOUT；LIVENESS_FAIL→拒绝+防伪告警；TIMEOUT/ERROR→不计数。删除 `score_high/mid` 与 WAIT_OTP「高置信度帧升级放行」逻辑（改为 pending 用户 OK 帧放行 + NO_MATCH 继续计数）。新增 `auth_fsm_last_reason()`。
3. `store/config`：`safe_policy_t` 增 `face_otp_after`(默认 3) / `face_verify_timeout_s`(默认 10) / `virtual_pin_enable`(默认 true)，删 `score_high/mid`；users.json 读取兼容旧字段（忽略）。`user_policy_set_score`→`user_policy_set_face`。
4. `rpc.c`：`set_threshold`→`set_face_policy`、`inject_score`→`inject_face`（reason 用字符串）。status `last_score`→`last_reason`。
5. 顺带：FR-9 对齐需求 v1.6（临时用户到期/次数耗尽→`user_del` 直接删除 + ALARM 审计日志，原为置停用）；`unlock_backend` 新增 `backend_verify_totp_any`（身份未定动态码，失败不累加用户级计数）；`safe_core` 显式链接 `safe_hal`；修 `platform.h` 头注释 `-Wcomment`。

**验收（实测）**：`tests/test_auth_fsm.c` 改写为 9 组按原因分流用例并通过；ctest **5/5 全绿**；增量重建 0 error、无新增告警。注：WAIT_OTP 中 NO_MATCH 继续计数会同时累加用户级失败计数（ADR-3 双轨），达 `max_failed` 触发该用户锁定后，`totp_any` 正确跳过被锁用户——测试已覆盖该交叉场景。`face_service_init`/`subscribe` 零调用（R6）留待步骤 3 接线，模拟器级注入在其前不可用。

### 步骤 2：camera_v4l2 后端 + 真实预览（1–2 天）　【✅ 已完成 · 2026-09-08】

1. ✅ 新增 `app/hal/camera/camera_v4l2.c`：V4L2 mmap 零拷贝、YUYV→RGB565 查表转换（整型无浮点）、`hal_camera_frame/release_frame` 语义（缓冲归后端所有；漏调 release 后端停更，契约已写进注释与实现）；后端运行期选择走 `camera_service`（环境变量 `SAFE_CAMERA_BACKEND`，默认 v4l2；**v4l2 初始化失败自动降级 null**，与 face_service 降级策略一致）；`SAFE_CAMERA_DEV` 可覆盖设备路径。**hal_camera.h 头文件未改**（§5.13 约定）。
2. ✅ `page_face` 预览区接真实帧：320×240 RGB565 画布 1:1（FR-16 采集=显示），四角扫描框 + 光带**叠加在画面之上**（FR-16「可叠加人脸框与状态提示」），首帧后隐藏「摄像头未配置」占位；50ms 拉帧 timer，页面隐藏时空转；`app.c` 补上 `hal_camera_init/start` 编排（此前全工程零调用）。
3. **验收（实测）**：
   - 模拟器（Xvfb+SDL）`SAFE_TEST_PAGE=FACE` 截图：人脸页显示真实摄像头画面，色彩正常无明显色偏；
   - 摄像头协商：YUYV 320×240、1/20fps、4 buffers mmap（Trust HDH Webcam，UVC）；
   - 稳定性：**连续 11 分钟 20fps 拉帧，进程存活、RSS 平稳 113352→113272 kB（零泄漏）、DQBUF 失败 0 次**（远超计划要求的 10 分钟）；
   - 降级：`SAFE_CAMERA_BACKEND=null` → 占位文字正常、不崩溃；
   - 构建无新增告警，ctest 5/5；
   - 顺带修复：预览帧缓冲定长 `uint16_t` RGB565（原 `lv_color_t` 数组在板子 32bpp 构建下 sizeof 翻倍会导致 memcpy 越界读 150KB——上板隐患提前消除）。
   - 说明：转换目前发生在主循环 tick（步骤 2 口径），步骤 3 引入 face 线程后由 face 线程 poll 该 fd 并转换（§3.4）。

### 步骤 3：face 线程 + FM225 协议状态机（2–3 天）　【⬜】

> **2026-09-10 细化**：本步拆为 3a / 3b / 3c 三个可独立验收的子步，**必须按序执行**。
> 拆分理由：face 线程的产出要经 `event_bus_post(EV_FACE_EVENT)` 交主线程，但当前通道
> 存在两处硬伤（payload 含 `const char *` 指针 → 跨线程 post 即悬空；`face_service_init`
> 零调用），若先起线程则要写两遍；故 **3a 先把通道修好**，3b 起线程，3c 补协议。

#### 3a：event_bus payload POD 化 + face 事件接线（0.5–1 天）　【✅ 已完成 · 2026-09-10】

**必须先改规约**（纪律 3：接口签名变更先改《技术路线规约》§5.8，再动代码）。

| 编号 | 内容 | 代码位置 | 验收 |
|---|---|---|---|
| a1 | `ev_auth_result_t` 的 3 个 `const char *` 改**定长字符数组**（`char evt[16]; char user[32]; char detail[64];`），全部 payload 结构消除指针 | `app/core/event_bus.h` | `grep -n "const char \*" app/core/event_bus.h` 在 payload 结构体内**零命中** |
| a2 | 新增 `EV_FACE_EVENT` 的 POD payload（`typedef struct { face_event_t ev; face_result_t res; face_enroll_result_t enroll; face_delete_result_t del; } ev_face_event_t;`），`face_event_t` 的 `FACE_EV_ERROR` 由 `const char *` 改定长 `char msg[64]` | `app/core/event_bus.h` + `app/hal/hal_face.h` | 结构体 `sizeof` 恒定、无指针成员 |
| a3 | `face_service_init()` 接线（`app.c` 调用，**此前全工程零调用**）；`face_service` 的**自有订阅数组拆除**，统一改走 `event_bus_publish/post`（规约 §5.18：全工程只留 worker + event_bus 两套机制） | `app/hal/face/face_service.c`、`app/app.c` | `grep -rn "face_service_subscribe" app/` 零引用或仅剩兼容壳；`on_face_event` unused 警告消失 |
| a4 | `auth_fsm` 订阅 `EV_FACE_EVENT`，收到 `FACE_EV_DETECT` 调 `auth_fsm_submit_face(id, reason)`（取代 UI/RPC 直塞） | `app/core/auth/auth_fsm.c` | 识别结果从「后端 → event_bus → auth_fsm」单一路径可达 |

**验收**：新增 `tests/test_event_bus.c`（跨线程 post 的 POD 按值拷贝正确、多订阅者收到、`unsubscribe` 后不再收到、主线程 `publish` 同步语义）；`ctest` 全绿；`SAFE_TEST_PAGE=FACE` + `SAFE_FACE_BACKEND=fake` 下 RPC `inject_face` 能驱动 FSM 状态变化（此前不可用）。
**提交**：`s3u3a: event_bus payload POD 化 + face 事件接线（R6）`

#### 3b：face 线程骨架 + 优雅退出（1 天）　【✅ 已完成 · 2026-09-10】

| 编号 | 内容 | 代码位置 | 验收 |
|---|---|---|---|
| b1 | 新建 face 线程：单线程 `poll()` **双 fd**（V4L2 video fd + UART fd），内部状态机分发 | 新建 `app/hal/face/face_thread.c`（+ `.h`），`app/app.c` 编排 init/deinit | 线程正常起停；`ls /proc/<pid>/task \| wc -l` **≤4**（规约 §3.3） — **✅ 实测**：新建 `face_thread.[ch]`，单线程 `poll()` 复用 `hal_camera_fd()`（§5.13 新增）+ UART fd（占位 -1，负 fd 被 `poll` 忽略，3c 接入）；起停正常。**应用逻辑线程 = 主+worker+MQTT+face = 4**；`face_thread_start` 确定性 +1（A/B 对照实测：带 face=23/21、去 face=22/20，mqtt on/off 各一）。OS 裸计数 23 另含 paho 内部 2 + Xvfb/llvmpipe 软件 GL ~18，非应用线程，口径见规约 §3.3 |
| b2 | **只转最新帧**：双帧缓冲 + 满则丢最旧，绝不阻塞采集；预览帧交接给主线程渲染 | `face_thread.c` | 高帧率轰炸下串口响应延迟不劣化（记实测值） — **✅ 实测**：相机后端 `v4l2_frame` 把驱动队列里就绪缓冲全部取出、除末帧外直接 `QBUF` 不转换，**只转最新一帧**；face 线程再拷进双帧缓冲 `s_fb[2]`（新帧覆盖旧帧/满则丢最旧，`fb` 锁发布），`page_face` 经 `face_thread_get_preview()` 读缓冲渲染——采集路径**永不阻塞**、**只保留最新帧**；`SAFE_TEST_PAGE=FACE` 截图实证真实摄像头帧经新链路渲染、视频/UI 双 FPS 正常。⚠️ 「串口响应延迟」实测**延后至 3c**（UART fd 当前为占位 -1，FM225 硬件未到，3b 无串口可测） |
| b3 | `face_service_tick` 轮询职责移交该线程（或保留 tick 作后端驱动，取简并写明） | `face_service.c`、`app.c` | 主线程 `app_tick_fast` 不再做帧转换 — **✅ 实测**：取简——**保留 tick 作后端驱动**（`face_service_tick` 仍在主线程 `app_tick_fast`，`app.c` 注释写明理由：当前 tick 仅驱动 fake 后端的软件注入、无 fd 可 poll，迁移收益低、改动面大），仅把**相机采集 + 帧转换**移入 face 线程；主线程不再拉帧/转换，`page_face` 只读 face 线程发布的最新帧（`face_thread_get_preview()`） |
| b4 | **优雅退出（R7）**：`SIGTERM`/`SIGINT` 处理只置 `g_quit`；`main` 主循环检测退出；`app_shutdown` 补 `worker_shutdown()`（§5.7）+ `platform_shutdown()`；face 线程 `pthread_join` | `app/main.c`、`app/app.c`、`app/core/support/worker.c` | `kill -TERM` 后进程**干净退出**（无 hang、`kill -0` 立即失败）；退出前 store 脏数据已 flush — **✅ 实测**：`main.c` 注册 SIGTERM/SIGINT，处理函数**只置** `volatile sig_atomic_t g_quit`（无 IO/alloc/printf，异步信号安全）；主循环 `while(!g_quit)` 跳出 → `app_shutdown()`：`face_thread_request_stop()` → `hal_camera_deinit()`（close fd）→ `face_thread_join()` → `face_service_deinit` → `mqtt_stop` → `worker_shutdown()`（§5.7 补实现：置标志+唤醒+join，不丢在途作业）→ `platform_shutdown()`。**`kill -TERM` 20/20 干净退出、0 hang、最大 0.48s（平均 <0.25s）**；store 经 tmp→fsync→rename 原子落盘、每次写即持久（R4/R5 未落地，无额外 flush 需求） |
| b5 | 线程退出时的 fd 释放顺序（先停流再关 fd 再 join，避免 DQBUF 卡住） | `face_thread.c`、`camera_v4l2.c` | 连续 20 次启停无 fd 泄漏（`ls /proc/<pid>/fd \| wc -l` 恒定） — **✅ 实测**：退出顺序严格 `STREAMOFF → close(fd) → pthread_join`（`hal_camera_stop` 先停流、`hal_camera_deinit` 再 close、最后 join，规避 `DQBUF` 卡死）；**20 次 `kill -TERM` 启停 fd 恒定 = 9、无泄漏、无 hang** |

**验收**：①✅ `kill -TERM` 干净退出（**重复 20 次全通过、0 hang、最大 0.48s**）；②✅ 连续 5 分钟运行 RSS 153480→156912 kB（**+2.2%**）、fd/线程恒定 **9/23**（应用逻辑线程恒 4；该 RSS 漂移经「null 后端」与「屏蔽 face_thread_start」两组对照实测，上升速率一致，判定为**既有、非本步引入**，已记入《开发进度.md》§5 风险）；③⬜ `valgrind --tool=helgrind` / `-fsanitize=thread` **未跑（环境未就绪）**，按本行约定**退化为「临界区审查 + 说明」**——`io` 锁（在途采集 vs `request_stop` 静默等待）、`fb` 锁（双缓冲发布翻转）、`s_quit` `volatile` 标志三处并发点已人工逐一审查，无共享可变状态裸读；建议步骤 4 补 TSan。
**提交**：`s3u3b: face 线程 poll 双 fd + 优雅退出链（R7）`

#### 3c：FM225 协议状态机 + socat 虚拟串口端到端（1–1.5 天）　【✅ 已完成 · 2026-09-11】

> **FM22x 硬件规格（据《FM22x 手册 V1.7》摘录，2026-09-11 核对入库）**
>
> | 项 | 规格 |
> |---|---|
> | 通讯接口 | UART（协议）+ USB（视频输出） |
> | UART 波特率 | **115200**（默认）；8 数据位 / 1 停止位 / 无校验 / 无流控 |
> | UART 信号电平 | **3.3V** |
> | UART 引脚 | 1=GND，2=UART_RX（模组接收），3=UART_TX（模组发送），4=VCC |
> | 供电 | **5.5V ~ 9.0V，≥1A**（⚠️ 不是 5V，规格下限 5.5V） |
> | USB 引脚 | 1=GND，2=DP，3=DM，4=VCC；**USB 输出彩色视频（UVC）** |
> | 协议帧 | `0xEF 0xAA`(Sync) + MsgID(1B) + Size(2B **大端**) + Data(N, 0≤N≤65535) + XOR(1B) |
> | XOR 算法 | 除 SyncWord 外**全部字节按位 XOR**；已用手册示例 `EF AA 00 00 05 13 00 00 03 1F 0A` 验算通过（Size=5 大端，XOR=0x0A ✓） |
> | 手册警告 | 模组**未上电时若 UART 已与外部设备连接，须把外部设备 UART 设为低电平**——上电顺序有讲究 |
>
> **真机联调采购清单（合计约 ¥200–400）**
> 1. FM22x 模组本体（含双目摄像头 + 灯板）
> 2. **9V/1A 电源**（或可调直流电源）——5.5V 是下限，USB 的 5V 不达标
> 3. **USB-TTL 转接器（3.3V 电平）**：CH340/CP2102/FT232 均可，⚠️ 必须切到 **3.3V** 档（5V 档会灌坏模组 UART）
> 4. 杜邦线若干
>
> **Ubuntu 侧识别方式**：USB-TTL → `/dev/ttyUSB0`；模组 USB 视频口 → `/dev/videoN`（UVC 标准摄像头）。
> 即 3c 的 socat 虚拟串口（`/tmp/fm225_host`）与 `SAFE_CAMERA_DEV=/dev/video0`，在真机上分别替换为
> `/dev/ttyUSB0` 与模组的 `/dev/videoN`，**业务代码零改动**——这正是「Ubuntu 先行」策略的收敛点。
> 注意模组 USB 视频输出分辨率可能非 320×240，真机联调前先用 `v4l2-ctl --list-formats-ext` 确认，
> 并按 `face_thread.c` 的分辨率校验告警（`02a6b38`）判断是否需调整预览缓冲尺寸（改 `FACE_THREAD_PREVIEW_W/H` 即可）。

> **前置注意（3b 交付后 QA 独立验证的遗留项，3c 必须处理）：**
>
> 1. **`face_service_emit` 无锁竞争（必改）**——`face_service.c` 里 `emit` 写 `s_last` / `s_seq` 未加锁。
>    3b 之前 `emit` 只在主线程调用（同步 `publish`），无竞争；**3b 之后 `face_service_emit` 已改走
>    `event_bus_post`，3c 的 FM225 解析在 face 线程里 emit 时会与主线程的 `face_service_last_result` 并发**。
>    3c 动手前先给这两个字段加锁（或用与 face 线程一致的发布机制收敛），并补一条并发用例。
> 2. **UART fd 接入点已就位**——3b 在 `face_thread.c` 留了 `face_thread_set_uart_fd(int fd)`
>    （仅 `start` 前可设，运行期不可改）；当前 `s_uart_fd = -1` 被 `poll` 忽略，`face_uart_drain()`
>    只做读走丢弃防缓冲涨满。3c 把 socat 的 `/tmp/fm225_host` fd 经此接入即可。
> 3. **上板运行时设 `MALLOC_ARENA_MAX=1`**——QA 已用四臂 A/B 归因确认 3b 的 RSS 缓慢增长是
>    glibc 多 arena 保留（非应用泄漏）；单核 A7 上设此值同时可减少 arena 竞争。


| 编号 | 内容 | 代码位置 | 验收 |
|---|---|---|---|
| c1 | **解析器做成纯函数/纯状态机**（`fm225_proto_feed(ctx, buf, n)` → 帧回调，不依赖 fd，便于单测） | `app/hal/face/fm225_proto.c`（+ `.h`） | 单测可脱离串口运行 — **✅ 实测**：`fm225_proto_[ch]` 7 状态逐字节推进（WAIT_SYNC0→SYNC1→MSGID→SIZE_H→SIZE_L→DATA→XOR），ctx 调用方分配、不碰 fd/全局；`fm225_proto_tick(ctx, now_ms)` 推进帧内超时（200ms 半帧作废），`fm225_proto_stats()` 回吐 bad/resync/timeout 计数。`tests/test_fm225_proto.c` 完全不依赖串口 |
| c2 | 按《FM22x 手册 V1.7》实现帧格式：帧头 `EF AA` + MsgID + 长度(2B 大端) + 数据 + XOR 校验；含**半包 / 粘包 / 坏校验 / 超时**处理 | `fm225_proto.c` | 单测覆盖 4 类边界 — **✅ 实测**：`test_fm225_proto.c` **7 组用例全绿**——①手册示例帧 `EF AA 00 00 05 13 00 00 03 1F 0A` 验算通过；②半包逐字节喂（中途不出帧）；③粘包三帧一次喂（Data 区含 `0xEF 0xAA` 不误触发帧头）；④坏校验（好帧+坏帧+好帧：只出 2 帧、bad=1）；⑤噪声重同步（孤立 `0xEF` 前导、resync≥1）；⑥超时（半帧 + tick 250ms 作废 to=1；20ms 内补齐不误杀）；⑦超长帧丢弃 + 空 Data 帧（Size=0） |
| c3 | `backend_fm225.c` 接上解析器：识别/录入/删除命令字与应答、超时重发；`SAFE_FACE_BACKEND=fm225` 可选 | `app/hal/face/backend_fm225.c` | 后端可切换、业务代码零改动 — **✅ 实测**：VERIFY(0x12)/ENROLL(0x13)/DELETE_USER(0x20) 命令拼帧下发；REPLY 应答受理策略——**VERIFY 一律受理**（模组单会话仅一条 VERIFY 在飞，迟到/跨重发应答比丢弃好），ENROLL/DELETE 须匹配 pending 防串话；命令应答 800ms 超时重发、3 次后 FACE_EV_ERROR 放弃；**verify 会话保活**（无 pending 且非录入中，2s 退避重开——重发放弃后链路不停摆）；模组侧断链修复实测：注入应答全丢→受理策略修正后恢复。`SAFE_FACE_BACKEND=fm225` 构建期可选 + `SAFE_FACE_BACKEND` 环境变量运行期可覆盖，业务代码零改动 |
| c4 | **socat 虚拟串口对**：backend 开 `/tmp/fm225_host`；注入脚本开 `/tmp/fm225_dev` | `tools/fm225_sim.py` | 双向可通 — **✅ 实测**：`socat -d -d pty,raw,echo=0,link=/tmp/fm225_host pty,raw,echo=0,link=/tmp/fm225_dev`；backend open host 端 + termios 115200 8N1（pty 侧 tcsetattr 部分参数不支持，非致命、字节流照通）；fd 经 `face_thread_set_uart_fd()` 在 face 线程 start 前接入（app.c 调序：face_service_init/start 移到 face_thread_start 之前）；脚本从 dev 端注入、backend 从 host 端收到，双向通 |
| c5 | 模拟帧源脚本注入：匹配 / 不匹配 / 活体失败 / 录入应答 / 心跳 | `tools/fm225_sim.py` | 可脚本化注入指定序列 — **✅ 实测**：`fm225_sim.py` 9 种序列（ready / no_match [n] / liveness / match [uid] / enroll / delete / bad / heartbeat / listen），`FM225_SIM_DEV` 可换设备；listen 模式实测打印出主控自动续发的 VERIFY(0x12) 帧 |
| c6 | 识别结果经 `face_service_emit` → `event_bus_post(EV_FACE_EVENT)` 交主线程（payload POD，3a 已备） | `face_thread.c`、`backend_fm225.c` | 主线程收到并驱动 FSM — **✅ 实测**：face 线程 poll UART 可读 → read → `fm225_backend_feed()` → 协议状态机 → `on_fm225_frame`（face 线程内）→ `face_service_emit`（**前置必改已落地**：emit 写 s_last/s_seq 持 `s_last_lock`，锁内只做字段读写、锁外 event_bus_post）→ 主线程 pump → auth_fsm `on_face_event` → FSM 分流 → UI 切页。并发用例 `test_event_bus.c` 用例 5（face 线程 2000 次 emit vs 主线程 last_result 撕裂检测）5 连跑全过 |

**验收**：①✅ `tests/test_fm225_proto.c` 7 组用例全绿（半包/粘包/坏 XOR/超时/噪声/超长/空 Data），ctest **7/7**；②✅ socat 注入「不匹配 ×3」→ FSM 转 `WAIT_OTP`（3 次 DENY「人脸未匹配」→ 60s 后「动态码超时」反证窗口成立），**全链路演示 + 截图**（`face_page.png` 相机画面 → `face_to_otp.png` / `wait_otp.png` 动态码键盘页）；③✅ 注入「活体失败」→ DENY + **ALARM「活体检测失败（防伪）」**日志；④✅ 注入匹配成功（uid=1）→ **UNLOCK admin via=face**；顺带：bad 帧序列注入后链路容错（后续好帧正常解出）；构建 0 error、**无新增告警**（全量重编告警均为基线既有，3c 触碰的 7 个源文件零告警）。
**提交**：`s3u3c: FM225 协议状态机 + socat 虚拟串口端到端`（实际 commit `931eaa3`，11 文件 +1400/-194）

> **真模组到货后**：只替换 c4 那一格（`/tmp/fm225_host` → `/dev/ttySx`）与波特率，其余零改动——这正是本计划的收敛目标。

### 步骤 4：性能验收与收尾（1 天）　【⬜】

1. `perf_probe` / `SAFE_PERF_LOG=1` 记录帧率与 CPU 占用；目标 **≥20fps @ 320×240**。
2. 「只转最新帧」压力验证：模拟源以高帧率轰炸，观察串口响应延迟与主循环耗时。
3. 实测数据回填《开发进度.md》与本文件；更新《技术路线规约》§4.2「先测量后优化」一节的实测值。

---

## 2. 纪律（对执行者——包括 Ubuntu 侧 Claude Code——的硬性要求）

1. **先读后动**：开工前先读《技术路线规约》v1.3 相关节与本计划；冲突时以规约为准并回改本计划。
2. **一步一记**：每完成一步 → 更新《开发进度.md》§4 指针 + 里程碑表加一行 + 本文件步骤打 ✅ 并附实测数据。
3. **接口先规约**：任何接口签名变更，先改《技术路线规约》§5，再动代码。
4. **不上板**：不碰 FBDEV / Buildroot / 交叉编译 / 设备树（属 Sprint2/3 上板段，另行计划）。
5. **提交纪律**：每步一个独立 git commit，信息格式 `s3u<N>: <摘要>`（Sprint3-Ubuntu 第 N 步）。

## 3. 当前状态指针

- ✅ Sprint1（PC/SDL 功能面：状态机/TOTP/存储/UI/MQTT-RPC，headless 验收 21/21）
- ✅ 技术路线规约 v1.3（2026-09-08 全量代码审计升级，备份 `归档/技术路线规约_v1.2_20260908.md`）
- ✅ FM225 手册入位 `~/桌面/资料/`
- ✅ 步骤 0（R1–R3 重构止血）——2026-09-08 完成，ctest 5/5
- ✅ 步骤 1（face_reason_t 接口切换）——2026-09-08 完成，ctest 5/5
- ✅ 步骤 2（camera_v4l2 + 人脸页实时预览）——2026-09-08 完成，11 分钟稳定性零泄漏，画面已实证
- ✅ 步骤 2.5 / 2.6（人脸页视频预览：填满面板→保持 4:3 居中最大化 + 双 FPS 分离显示）——2026-09-10 完成
- ✅ 步骤 3a（event_bus payload POD 化 + face 事件接线 R6）——2026-09-10 完成，commit `9210555`，ctest 6/6
- ✅ 步骤 3b（face 线程 + 只转最新帧 + 优雅退出链 R7）——2026-09-10 完成，commit `e7ac93f`，ctest 6/6；20 次 kill-TERM 干净退出、fd 恒定 9
- ✅ 步骤 3c（FM225 协议状态机 + socat 虚拟串口端到端）——2026-09-11 完成，commit `931eaa3`，ctest 7/7（新增 test_fm225_proto 7 组用例 + test_event_bus 并发用例）；3×不匹配→WAIT_OTP、活体失败→ALARM、匹配→UNLOCK via=face、坏帧容错全链路实测 + 截图
- ✅ 步骤 3a（event_bus POD 化 + face 接线）→ ✅ 步骤 3b（face 线程 + 优雅退出）→ ✅ 步骤 3c（FM225 协议 + 端到端）
- ⬜ **当前：步骤 4（性能验收与收尾）——未开工**











