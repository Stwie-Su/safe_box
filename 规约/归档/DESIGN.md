# DESIGN.md — 智能保险柜 UI 需求与设计说明

> 版本：v1.0（PC 验证阶段）　目标硬件：i.MX6ULL（LCD 1024×600 / RGB565）　UI 框架：LVGL v9
> 本期范围：**日志、用户管理、数据存储、UI 设计**。摄像头、开锁功能暂不做。

---

## 1. 需求基线（已与用户确认）

| 项 | 决定 |
|---|---|
| 用户模型 | 多用户 + 角色（admin / user），数据模型从一开始按多用户设计，避免返工 |
| 认证方式 | 纯数字 PIN（4–8 位）；`auth_method` 字段预留 `"otp"`（后续接手机动态码） |
| 日志入口 | 独立一级页（与主页 / 设置并列），便于随时审计 |
| 数据存储 | JSON 文件 + 统一抽象存储层；暂不引数据库、暂不建独立根文件系统 |
| 设置入口 | 密码 / 用户 / WiFi 收敛到「设置」中枢；进入需二次验证（管理员 PIN） |
| 主题 | 4 套可切换：石墨黑 / 月白 / 蓝白 / 松石青 |

---

## 2. 用户管理

### 2.1 设计原则
- 保险柜是安全产品，敏感操作（改密码 / 删用户 / 改策略）必须**二次验证**后才可见可改。
- 数据模型从 v1 起按多用户 + 角色设计，不预留"单用户"分支。

### 2.2 数据模型（`/var/lib/safe/users.json`）
```json
{
  "version": 1,
  "policy": { "pin_min_len": 4, "pin_max_len": 8, "max_failed": 5, "lock_seconds": 60 },
  "users": [
    {
      "id": 1, "name": "owner", "role": "admin",
      "pin_hash": "<PBKDF2-SHA256 hex>", "pin_salt": "<hex>",
      "auth_method": "pin", "enabled": true,
      "created_at": "2026-08-25T10:00:00",
      "failed_attempts": 0, "lock_until": 0
    }
  ]
}
```

### 2.3 安全与约束
- **PIN 只存哈希**：`PBKDF2-SHA256(pin + pin_salt)`，每用户随机盐；验证时现算比对，绝不存明文/可逆加密。
- **防暴力**：连续错 `max_failed` 次，锁定 `lock_seconds` 秒（`failed_attempts` + `lock_until`）。
- **权限边界**：改密码必须验旧 PIN；删除用户 / 修改策略仅 `admin` 可操作。
- **操作审计**：增删改均写一条日志（操作者 / 目标 / 结果）。

---

## 3. 日志设计

### 3.1 格式
- **JSON Lines（每行一条 JSON）**，明文追加；易解析、易筛选、易导出。
- 字段：`ts`(ISO8601) / `evt`(事件类型) / `user` / `res`(`ok`|`fail`) / `detail`。

### 3.2 事件枚举
| evt | 含义 |
|---|---|
| `unlock` | 开锁成功 |
| `unlock_fail` | 开锁失败（含尝试次数） |
| `lock` | 上锁 |
| `user_add` / `user_del` / `user_modify` | 用户增 / 删 / 改 |
| `pwd_change` | 修改密码 |
| `wifi_change` | WiFi 配置变更 |
| `setting_change` | 其他设置变更 |
| `factory_reset` | 恢复出厂（预留） |
| `tamper` / `power_loss` | 撬动 / 异常断电（后续硬件接入预留） |

### 3.3 存储与保留
- 追加写；超过 N 条或 N 天时滚动截断（保留最近 N 条 / 转 `safe.log.1`），防写爆存储。
- 审计页支持按 事件 / 结果 / 时间 筛选；导出接口（USB / 网络）先预留。

---

## 4. 数据存储

### 4.1 方案
- **JSON 文件 + 抽象存储层**，目录 `/var/lib/safe/`（PC 阶段落在 rootfs 该目录；板子权限 600、属主 root）。
- 选型理由：用户个位数、日志千条级，数据量极小，SQLite 属过度设计且多依赖；抽象层保证将来换 SQLite / 独立分区时业务代码零改动。

### 4.2 三份文件与安全分级
| 文件 | 内容 | 处理方式 |
|---|---|---|
| `users.json` | 用户 / 角色 / PIN 哈希 / 盐 | 哈希（不可逆），权限 600 |
| `network.json` | SSID / 加密方式 / `psk_enc` | AES-256-CBC 加密（可逆，系统联网需解密） |
| `safe.log` | 审计日志 | 明文追加，滚动截断 |

### 4.3 存储层接口（C，业务只调这些）
```c
/* 用户 */
int user_load_all(safe_user_t **list, int *count);
int user_add(const safe_user_t *u);
int user_del(int id);
int user_verify_pin(const char *name, const char *pin);     /* 0=通过 */
int pin_hash(const char *pin, uint8_t *salt, char *out_hex);
int pin_check(const char *pin, const char *salt_hex, const char *hash_hex);

/* 网络（psk 加密可逆） */
int net_add_wifi(const char *ssid, const char *sec, const char *psk);  /* 内部 AES-256 加密 */
int net_get_psk(const char *ssid, char *psk_out);                      /* 内部解密 */

/* 日志 */
int log_append(const char *evt, const char *user, int res, const char *detail);
int log_query(const char *filter, log_entry_t **out, int *count);      /* 可选 */

/* 设备密钥（PC 阶段固定串，后续入安全元件） */
int devkey_get(uint8_t *key, size_t *len);
```

---

## 5. UI 信息架构

```
主界面（锁状态 · 时钟 · 开锁占位）
 ├─ 日志 · 记录（独立一级页，随时审计）
 └─ 设置（进前二次验证 admin PIN）
      ├─ 用户管理（列表 / 添加 / 改密 / 删除 / 启用）
      ├─ 网络（WiFi 扫描 / 连接 / 已存网络）
      └─ 系统（时间日期 / 安全策略 / 恢复出厂）
```
- 底部 Tab 导航：〔主页 │ 日志 │ 设置〕；顶部状态栏放锁图标 + 时钟 + 信号。
- 列表项用大卡片，触控友好（1024×600 屏）。

---

## 6. 主题系统

### 6.1 四套主题色板（LVGL 直接用 hex）
| 角色 (LVGL) | 石墨黑 Graphite | 月白 Moonlight | 蓝白 Azure | 松石青 Pine |
|---|---|---|---|---|
| bg 背景 | `#14161A` | `#F3F0EA` | `#F2F6FB` | `#0F1719` |
| panel 面板 | `#1E2127` | `#FFFFFF` | `#FFFFFF` | `#16242A` |
| panel2 次面板 | `#262A31` | `#EDE9E1` | `#E9F0F8` | `#1E3036` |
| text 正文 | `#E9EBEE` | `#1B1D21` | `#16222E` | `#E2EEF0` |
| text_mut 次要 | `#9AA0A8` | `#6E6A62` | `#6B7B8C` | `#8AA0A4` |
| border 描边 | `#33383F` | `#DED8CD` | `#D5DEE8` | `#2A3D43` |
| accent 强调 | `#C8A45C`(黄铜) | `#B08D3E` | `#2D6FB3`(钢蓝) | `#3FB6A8`(松石) |
| accent_ink 强调上文字 | `#1A1407` | `#FFFFFF` | `#FFFFFF` | `#06201D` |
| ok | `#5FB37A` | `#3F8F5C` | `#3F8F5C` | `#5FB37A` |
| warn | `#D9A441` | `#B5862C` | `#C0882A` | `#D9A441` |
| danger | `#D2584F` | `#B23A33` | `#C0493F` | `#D2584F` |

### 6.2 实现要点
- 颜色集中：`app_theme_t THEMES[4]` + 可复用 `lv_style_t`（`st_screen/st_panel/st_text/st_accent_btn/st_border`）。
- 页面控件只引用 style，绝不写死 hex；切主题 `theme_switch(idx)` → `lv_obj_report_style_change()` 全局刷新。
- 加主题只需在 `THEMES[]` 增一行；"设置 → 主题"项可让用户自选。

---

## 7. UI 页面清单
1. **主页**：锁状态图标 + 时钟 + 「开锁」占位按钮（本期不实现）+ 底部 Tab。
2. **设置**（进前二次验证 admin PIN）：列表〔用户管理 │ 网络 │ 系统〕。
3. **用户管理**：用户列表（名称 + 角色 + 启用态）+〔添加〕；点用户→详情（改密 / 删除 / 启用）。
4. **改密弹窗**：验旧 PIN → 输新 PIN（两次）→ `lv_keyboard`。
5. **网络**：WiFi 扫描列表 + 已存网络 + 连接输 PSK（键盘）。
6. **系统**：时间/日期 + 安全策略（失败次数 / 锁定时长）+ 恢复出厂（二次确认）。
7. **日志**：列表（时间 + 事件 + 用户 + 结果）+ 筛选 + 导出（预留）。

---

## 8. 待确认 / 下一步
- 锁定策略默认值（max_failed / lock_seconds）、日志保留条数 → 待用户拍板。
- Craft 落地顺序：`theme.h/.c`（4 套调色板 + 复用样式 + `theme_switch`）→ 三个页面骨架 → 修复「改密键盘弹不出」（`lv_keyboard_set_textarea` 绑定）。
