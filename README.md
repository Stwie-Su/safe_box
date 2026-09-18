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
- 远程指令权限分级（MQTT 凭据 + rpc_guard 去重/鉴权）
- 四套可换调色板主题（Graphite 深色 / Moonlight 暖光 / Pine 青绿 / Azure 蓝白）
- 相机预览（UVC）、模组健康与降级、启动对账

## 构建

```bash
# PC 验证（SDL2 后端）
./build_pc.sh

# 交叉编译（板子）
mkdir -p build_board && cd build_board
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/imx6ull.cmake ..
make
```

## FM225 人脸识别模组

FM225 的识别在模组端完成，板子通过 UART 收发指令帧。本项目 `app/hal/face/` 下的协议
状态机与 `tools/` 下的自检/模拟脚本为**自行逆向的成果**。海凌科官方手册与官方调试工具为
厂商私有资料，**未包含在本仓库**，请向厂商获取。

## 许可证

- 本项目（仓库根目录）：MIT License —— 见 [LICENSE](LICENSE)
- `lvgl/` 为 vendored 的 LVGL v9，沿用其自身的 MIT License（Littlev Graphics Library）
