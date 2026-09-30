# R9 自研 MQTT 客户端 —— 进度与中断恢复点

> 每次推进都更新本文件。**中断后从「当前步骤」继续即可，不必重读对话。**
> 最后更新：2026-09-21 23:55

## 一、已完成

| 任务 | 内容 | commit |
|---|---|---|
| T01 | 协议编解码层 `mqtt_codec.c/h` + 单测（含半包/粘包） | `0346d4c` |
| T02 | 未确认报文队列 `mqtt_inflight.c/h` + 单测 | `3bdbd12` |
| T03 | 连接状态机 `mqtt_fsm.c`（纯逻辑、时间注入） | 已提交 |
| T04 | 客户端 `mqtt_client.c` + `tls_stream`（后端 A 明文）+ `rpc_guard` + LWT | `e92946a` |
| T06 前置 | **`tls_stream` 句柄式重构** + `fd` / `want_write` 接口 | `6221f4c` |

**实机已验证**（每次改动后都应重跑）：
```
[MQTT] connected (connack=0)
mosquitto_sub -t 'safe/#' → safe/status {"state":"IDLE","mqtt_online":1,...}
```
本机 broker：`/usr/sbin/mosquitto -c /etc/mosquitto/mosquitto.conf`，监听 1883。

## 二、架构要点（别搞错顺序）

- **TLS 属于 T06，不是 T04**。`tls_stream.h` 注释写明双后端：
  - 后端 A = 恒等（明文 TCP 非阻塞）**已落地**
  - 后端 B = mbedTLS **T06**
- 两条钉死的语义（写在头文件里，实现时别违反）：
  1. `tls_stream_read()` 非阻塞，外层 poll 确认可读后才调，net 线程恒传 `timeout_ms=0`
     （否则内外两层超时打架，keepalive 被吞）
  2. **握手期事件掩码由外层按 `tls_stream_want_write()` 动态决定** ——
     TLS 握手时 mbedTLS 常返回 `WANT_WRITE`，写死 POLLIN 会**永久卡死**
- `SAFE_FEATURE_TLS` 若开启但未实现 → **编译期 `#error`**（绝不静默退化成明文）

## 三、当前步骤：**T06 后端 B（mbedTLS）**

### 步骤 1 — 实现 `tls_stream.c` 的 TLS 分支 ✅ **已完成**
（自检：`-DSAFE_FEATURE_TLS` 与明文分支均 `-Wall -Wextra -Werror` 零告警通过）
实现要点：ssl/conf/x509/pk/entropy/ctr_drbg 全放进句柄；
connect 先 TCP 再 `mbedtls_ssl_set_bio(&s->ssl, &s->fd, ...)`；
connect_poll 两段式（TCP SO_ERROR → TLS handshake）；
handshake/read/write 的 WANT_READ/WANT_WRITE 全部翻译成 want_write 供外层掩码。
在 `struct tls_stream` 里放 mbedTLS 上下文，实现：
- `tls_stream_new()`：`cfg->enable` 为真时初始化 entropy/ctr_drbg/ssl/conf/证书
- `tls_stream_connect()`：先非阻塞 TCP，**连上后**把 fd 交给 mbedTLS 并启动握手
- `tls_stream_connect_poll()`：TCP 阶段（POLLOUT + SO_ERROR）→ TLS 阶段
  （`mbedtls_ssl_handshake()`：0=完成；`WANT_READ/WANT_WRITE`→ 置 `want_write` 并返回 1）
- `tls_stream_read/write()`：`mbedtls_ssl_read/write`，
  `WANT_READ` → -1（暂无数据），`WANT_WRITE` → 置 want_write 并 0/-1
- `tls_stream_close/free()`：`mbedtls_ssl_free` + 各上下文释放（**注意 free 顺序**）

### 步骤 2 — CMake 开关与链接 ✅ **已完成**（已提交）
`cmake/SafeFeatures.cmake`：`option(SAFE_FEATURE_MQTT_TLS)` + 探测 mbedTLS
（ssl.h / libmbedtls / libmbedx509 / libmbedcrypto），**找不到就 FATAL_ERROR**；
找到则 `add_compile_definitions(SAFE_FEATURE_TLS)` 并导出 `SAFE_MBEDTLS_LIBS`。
`app/core/CMakeLists.txt`：`target_link_libraries(... ${SAFE_MBEDTLS_LIBS})`。
自检：默认 17/17；`-DSAFE_FEATURE_MQTT_TLS=ON` 找到 mbedTLS、零告警、17/17。
- `cmake/SafeFeatures.cmake`：加 `option(SAFE_FEATURE_MQTT_TLS ...)`
- 找到 mbedTLS（`find_path/find_library`）→ 链接 `mbedtls mbedx509 mbedcrypto`
- **找不到就报错**，不要静默退化
- 目标定义 `SAFE_FEATURE_TLS`

### 步骤 3 — 证书与 broker ✅ **已完成**
- 用 openssl 生成 CA / 服务端 / 客户端证书（**注意 notBefore**，板子 RTC 电池已装但仍需校时一次）
- mosquitto 加 8883 监听（`require_certificate` 视双向 TLS 需求而定）

### 步骤 4 — 验证 ✅ **已完成**
- PC：`SAFE_FEATURE_MQTT_TLS=ON` 编译 → 连 8883 → `mosquitto_sub --cafile` 验证真的加密
- **关键判据**：抓包/日志里不能出现明文 MQTT 报文；握手失败要能退避重连而不是卡死

### 步骤 5 — ARM 交叉编译 mbedTLS ✅ **已完成**
- 板子侧目前**没有** mbedTLS 库，需交叉编译（约 60~100KB）
- 工具链：`arm-linux-gnueabihf-gcc`（`~/100ask_imx6ull-sdk/`）

## 四、环境备忘

- PC 已装 **mbedTLS 2.28.0**（`/usr/include/mbedtls`, `libmbedtls.so`）
- SSH 无 TTY：`sudo` 必须写 `echo '123456' | sudo -S ...`
- 构建：`cmake --build build_pc -j4`；板子：`cmake --build build_board -j4`
  （交叉构建须加 `-DPython3_EXECUTABLE=/usr/bin/python3`）
- 质量门：编译**零告警** + `ctest` **17/17** + 板子零告警


---

## 五、步骤 2~4 完成纪实（2026-09-22）

### 步骤 2：CMake 开关

- `cmake/SafeFeatures.cmake`：`option(SAFE_FEATURE_MQTT_TLS ...)` 默认 **OFF**；
  开启时 `find_path/find_library` 探测 mbedTLS（ssl / x509 / crypto），
  **找不到直接 FATAL_ERROR**（绝不静默退化成明文），找到则定义 `SAFE_FEATURE_TLS`
  并导出 `SAFE_MBEDTLS_LIBS`。
- `app/core/CMakeLists.txt`：`target_link_libraries(safe_core ... ${SAFE_MBEDTLS_LIBS})`。

### 步骤 3：证书与 broker（★ 两个坑）

1. **坑一：自签证书必须带 SAN。** 连 `127.0.0.1` 时 CN=localhost 不够，
   mbedTLS/openssl 会报 `host name verification failed`。
   签发时加：`subjectAltName=DNS:localhost,IP:127.0.0.1`。
2. **坑二：私钥权限。** mosquitto 以 `mosquitto` 用户运行，私钥属主是 book 时
    broker 起不来。证书统一放 `/etc/mosquitto/certs/`，
   `chown mosquitto:mosquitto` + `chmod 644(crt) / 640(key)`。
3. **坑三：`/tmp` 是 tmpfs，重启/清理后证书会消失。**
   真源是 `/etc/mosquitto/certs/ca.crt`，测试前 `cp` 回来即可。

### 步骤 4：★ 致命坑 —— TLS 握手永不推进

**现象**：TCP 连上了，但永远不发 MQTT CONNECT，也没有任何报错。
broker 日志只有一行 `Client <unknown> closed its connection`，极难定位。

**根因**：`mqtt_client.c` net 线程第 6 步

```c
if (g_connecting && g_sock >= 0 && (fds[0].revents & POLLOUT)) {
    int r = tls_stream_connect_poll(g_ts, 0);
    g_connecting = 0;          /* ← 无条件清零 */
    ...
}
```

- 明文 TCP：`connect_poll` 第一次就返回 0（TCP 就绪即完成）→ **从没暴露**；
- TLS：第一次必然返回 **1**（握手刚起步、停在 ClientHello 之后）→ `g_connecting`
  被清零 → 第 6 步再不执行 → **握手永远没有第二轮**；
- 而且就绪判据只认 `POLLOUT`，握手期多数时候是「想读」（等 ServerHello）。

**修法**：`r == 1` 时保持 `g_connecting = 1` 等下一轮；就绪判据同时受理 `POLLIN`，
是否要 `POLLOUT` 由 `tls_stream_want_write()` 动态决定。

> 教训沉淀：这类「新增一条异步子流程」的 bug，在旧路径上永远测不出来。
> 判据不能只看「有没有报错」，要看**子流程有没有真的推进到下一步**
> （这里的可观测抓手就是握手成功的打印 + broker 日志有没有出现 client id）。

### 步骤 4：验收结果

- 默认 OFF 构建：零告警、`ctest` 17/17、明文 1883 正常。
- TLS ON 构建：零告警、17/17、`ldd` 可见 libmbedtls/libmbedx509/libmbedcrypto、
  flags 里确有 `SAFE_FEATURE_TLS`。
- 端到端：`[TLS] 握手完成：TLSv1.2 / TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384`
  → `[MQTT] connected (connack=0)` → `safe/status` 上行（mqtt_online=1，5s 一次）
  → `safe/cmd` 下行 → `safe/log` 回执，**全部走加密链路**。
- **失败闭合**：OFF 构建强制 `SAFE_MQTT_TLS=1` →
  打印「TLS 通道创建失败：构建未启用 mbedTLS 或证书无效，远程通道停用」，
  订阅端收不到任何消息（证明没有静默退回明文）。

### 环境备忘（本轮新增）

- **SDL dummy 用不了**：`SDL_VIDEODRIVER=dummy` 会报
  `Failed to create SDL renderer 'Couldn't find matching render driver'`。
  必须起 Xvfb：`Xvfb :99 -screen 0 1024x600x24 &`，然后 `DISPLAY=:99`。
- 跑应用的推荐姿势：`timeout 20 stdbuf -oL -eL ./build_pc/bin/lvglsim`
  （stdout 重定向到文件是全缓冲，不加 stdbuf 会在被 kill 时丢日志）。
- **判据陷阱**：`safe/status` 是 **retain** 消息，ctest 也会在 1883 上留一条。
  验证前先 `mosquitto_pub -t safe/status -r -n` 清掉，
  否则会把上一次的残留当成"本次连上了"的证据（本轮差点被它误导）。

### 下一步

- 步骤 5：ARM 交叉编译 mbedTLS（板子没有）。工具链 `arm-linux-gnueabihf-gcc`。


---

## 六、步骤 5 完成纪实（2026-09-22）

### 交叉编译 mbedTLS（静态库）

- 源码：mbedTLS **2.28.8**（与 PC 的 2.28.0 同代，API 兼容）。
  **VM 无外网**（DNS 能解析 github.com，但 curl 连不上），源码从 Windows 宿主机
  `curl` 下来再用 SFTP 传到 VM。
- 工具链是 **`arm-buildroot-linux-gnueabihf-gcc`**（不是 Linaro 的
  `arm-linux-gnueabihf-gcc`），位于
  `~/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/host/bin/`。
- 命令：`make -j4 lib CC=... AR=... CFLAGS="-O2 --sysroot=$SYSROOT"`
  → `libmbedcrypto.a 545KB / libmbedtls.a 242KB / libmbedx509.a 130KB`，
  零告警，`objdump -f` 核对为 **armv7**。
- 装进 sysroot（`/usr/include/mbedtls` + `/usr/lib/*.a`），
  CMake 的 `find_path/find_library` 直接就能找到，**无需改探测逻辑**。

### ★ 顺带发现：板子根本没有 cJSON（比 mbedTLS 更要紧）

排查「为什么板子二进制里没有 mbedtls 符号」时发现：
`SAFE_HAVE_CJSON == FALSE` → `app/core/CMakeLists.txt` 走 `else()` 分支，
编的是 `mqtt_stub.c` + `rpc_stub.c`。也就是**板子上远程通道一直是永久断线的**，
JSON 存储后端也是「受限模式」。

补法：cJSON 1.7.18 单文件，同样交叉编译成静态库装进 sysroot
（头文件放 `usr/include/cjson/cJSON.h`，与 PC 的 `PATH_SUFFIXES cjson` 对齐）。

### ★ 顺带修掉一个过时默认值：交叉编译时 MQTT 不再默认关闭

`cmake/SafeFeatures.cmake` 原逻辑：

```cmake
if(CMAKE_CROSSCOMPILING)
    set(_mqtt_default  OFF)   # 「避免误链 host 库」
```

这是当年为绕开「板子没有 ARM 版第三方 MQTT 库」而设的。R9 自研之后 MQTT
**不再依赖任何第三方库**，唯一前置只剩 cJSON —— 该默认值若继续保留，
板子会静默编进 `mqtt_stub.c`，R9 的全部收益落空。已改为一律 `ON`
（能不能编真实现交给 `SAFE_HAVE_CJSON` 判断）。

### 板子构建结果

| 指标 | 修之前 | 修之后 |
|---|---|---|
| `SAFE_FEATURE_MQTT` | OFF（走 stub） | **ON（真实现）** |
| cJSON 符号 | 0 | **80** |
| tls_stream 符号 | 0 | **9** |
| mbedtls 符号 | 0 | **955** |
| 二进制大小 | 4.50 MB | **5.15 MB**（+650KB） |
| 编译告警 | 0 | **0** |

### 下一步（尚未做）

- 上板实测：ADB 推固件 → 起 mosquitto → 验证板子经 TLS 连通。
  ⚠️ 注意：这一改动**改变了板子运行时行为**（多了 net 线程、会主动连 broker），
  刷机前应先确认板子 broker 地址与凭据配置。
