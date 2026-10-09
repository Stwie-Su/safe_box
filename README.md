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
├─ tests/          ctest 用例（纯逻辑、可确定性验证）
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
**测试**：`ctest --test-dir build_pc`（24 项）。
**文档入口**：面向人看 README；面向 agent 看 `CLAUDE.md`（含目录禁区与省上下文纪律）；
需求/设计看 `规约/需求规约.md` + `规约/技术路线规约.md`；排错先看 `规约/Bug案例集.md`（定位方法论），
缺陷清单查 `规约/归档/Bug台账_全量.md`。

厂商资料（PDF）不入库，已移至 `~/桌面/中间产物/06_资料/`。
