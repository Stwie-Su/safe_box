# UI 全面重设计（左 rail 导航 + 图标字体 + 统一过渡 + 卡片化）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `claude_prompt_ui_redesign.md` 把底部 tab 导航的旧 UI 重设计为左侧图标 rail + 图标字体 + 统一 200ms 横向过渡 + 卡片式仪表盘，并同步规约与进度文档。

**Architecture:** 图标用 `lv_font_conv` 从 Lucide SVG 子集生成 `lv_font_cn 之外` 的独立图标字体 `lv_font_icon.c`（码位映射在私有区 0xE000+），封装 `icon_label()` helper；`ui.c` 外壳重构为「顶栏 + 左 rail + 内容区」（rail 常驻、页面切换走 `lv_screen_load` 之外的既有常驻 roots + 横向过渡复用现有 `page_anim_out/in` 机制改参数）；页面逐个卡片化对齐预览 HTML 布局。

**Tech Stack:** LVGL v9、CMake、lv_font_conv（VM 已装 1.5.3，路径 `/usr/bin/lv_font_conv`）、node v20（SVG 拉取不需要，SVG 直接手写内嵌于生成脚本）

**Spec:** `claude_prompt_ui_redesign.md`（本仓库根目录，含 §0.0 用户已裁定决策）；《技术路线规约》v1.3 §5；《开发进度.md》§0/§4

## Global Constraints

- 工程唯一真源 `~/桌面/lv_port_linux`，分支 `refactor/skeleton`；禁提交 `CLAUDE.md` / `*.bak` / `.claude/`（`git add <精确文件>`，禁 `git add -A`）
- 颜色一律 `theme_color(TH_*)` + `st_*` 样式，不写死 hex；四套主题必须可切换（验收项 7）
- 线程 ≤4：过渡必须复用 lv_timer / lv_anim，不新建线程、不每帧 malloc（回调里禁 create/free）
- 分层纪律：ui 只经 event_bus 订阅；`tools/check_layers.sh` 必须过
- 首版主题 = Moonlight 暖光（`theme.c` 月白，即 THEMES[1]）；`theme_init` 默认 `g_idx` 由 4（浅蓝）改为 1（月白）
- 性能：HOME 稳态 ≥20fps（板子基线 21fps），新增过渡劣化 >15% 判不过
- SYSTEM 页「恢复出厂设置」按钮必须完整可见可点（用户裁定 #3）
- 每步 commit 前缀 `ui-redesign:`，且每步先改《技术路线规约》对应小节再动码（CLAUDE.md 纪律 1/3）
- 每完成一个任务立即更新《开发进度.md》§0 + §4 + 里程碑，commit 规约 `s3u<N>` 之外本轮统一用 `ui-redesign:` 前缀（规约文档内登记为步骤 5 的子任务）

---

### Task 1: 图标字体资产生成 + icon_label helper

**Files:**
- Create: `tools/gen_icon_font.py`（生成脚本，一次性工具）
- Create: `app/ui/fonts/lv_font_icon.c`（生成产物，~35 个 glyph，估 <40KB）
- Create: `app/ui/fonts/lv_font_icon.h`（码位宏 + 字体声明）
- Modify: `app/ui/CMakeLists.txt`（GLOB 已含 fonts/*.c，无需改——确认即可）
- Modify: `app/ui/icons.h`（加 `ui_icon_font_t` 或直接用 lv_font_icon.h；保留旧矢量图标 API 不删）

**Interfaces:**
- Produces: `const lv_font_t * app_icon_font(void);`（返回图标字体，单位 glyph 32px 基准，bpp 4）
- Produces: `app/ui/fonts/lv_font_icon.h` 中的码位宏 `ICON_LP_HOME` `ICON_LP_USER` `ICON_LP_LIST` `ICON_LP_FACE` `ICON_LP_WIFI` `ICON_LP_SETTINGS` `ICON_LP_SYS` `ICON_LP_LOCK` `ICON_LP_LOCK_OPEN` `ICON_LP_KEY` `ICON_LP_KEYPAD` `ICON_LP_BELL` `ICON_LP_PLUS` `ICON_LP_BATTERY` `ICON_LP_SHIELD` `ICON_LP_CAMERA` `ICON_LP_UNDO` `ICON_LP_POWER` `ICON_LP_TRASH` `ICON_LP_EDIT` `ICON_LP_CHEV_RIGHT` `ICON_LP_CHEV_LEFT` `ICON_LP_CHECK` `ICON_LP_X` `ICON_LP_REFRESH` `ICON_LP_SEARCH` `ICON_LP_CLOCK` `ICON_LP_DATABASE` `ICON_LP_LOGOUT` `ICON_LP_INFO` `ICON_LP_WARN` `ICON_LP_FINGER`（33 个，覆盖 rail 7 + 状态/列表/按钮/弹窗）
- Produces: `lv_obj_t * icon_label(lv_obj_t * parent, uint32_t icon_cp, const lv_font_t * font, lv_color_t color);`（一个 label，text = 单码位 UTF-8 编码）

**注意（上轮踩坑规避）：** 图标字体上色走 `style.text.color`（glyph 是普通字符渲染，天然支持）；生成命令必须 `--bpp 4 --no-compress`。

- [ ] **Step 1: 写生成脚本 `tools/gen_icon_font.py`**

```python
#!/usr/bin/env python3
"""从内嵌 Lucide SVG 生成 LVGL 图标字体（claude_prompt_ui_redesign.md §0.0 裁定 1）。
用法：python3 tools/gen_icon_font.py   （在仓库根目录运行）
产物：app/ui/fonts/lv_font_icon.c / lv_font_icon.h
码位：私有区 0xE000 顺序分配，宏名与 ICON_LP_* 一一对应。
"""
import subprocess, os

# Lucide 0.4xx 的 24×24 stroke SVG path（hand-off 自 lucide.dev，每个 icon 一条 path/一组 shape，
# viewBox="0 0 24 24"，fill=none stroke=currentColor stroke-width=2 —— lv_font_conv 接受 SVG 输入）
SVGS = {
    "ICON_LP_HOME": '<path d="m3 9 9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/>',
    "ICON_LP_USER": '<path d="M19 21v-2a4 4 0 0 0-4-4 4 4 0 0 0-4-4 4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>',
    # ... 33 个，逐个从 lucide.dev 复制 path 数据（执行者补充完整表，禁止省略）
}
START_CP = 0xE000
# 生成：每个 icon 写临时 .svg（外面包 <svg xmlns ... viewBox="0 0 24 24" fill="none"
# stroke="black" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">），
# 逐个 --font xxx.svg --range 0xE0NN，合并多个 --font/--range 对到一次 lv_font_conv 调用。
```

完整实现要点：脚本内建 33 个 SVG path 常量表（lucide home/user/users/list/scan-face/wifi/settings2/cpu/lock/lock-open/key/square-user-round(键盘格)/bell/plus/battery-medium/shield/camera/undo-2/power/trash-2/pencil/chevron-right/chevron-left/check/x/rotate-cw/search/clock/database/log-out/info/triangle-alert/fingerprint），写临时 svg → `lv_font_conv --size 32 --bpp 4 --no-compress --font a.svg -r 0xE000 --font b.svg -r 0xE001 ... --format lvgl -o app/ui/fonts/lv_font_icon.c`，同时产出头文件（宏→码位）。

- [ ] **Step 2: 运行脚本生成字体并核对产物**

Run: `cd ~/桌面/lv_port_linux && python3 tools/gen_icon_font.py && ls -la app/ui/fonts/lv_font_icon.*`
Expected: 两个文件生成，`.c` < 60KB，`.h` 含 33 个 `#define ICON_LP_* 0xE0XX`

- [ ] **Step 3: 写 `icon_label()` helper（app/ui/icons.h 声明、icons.c 实现）**

```c
/* icons.h 追加 */
#include "fonts/lv_font_icon.h"
const lv_font_t * app_icon_font(void);
lv_obj_t * icon_label(lv_obj_t * parent, uint32_t icon_cp,
                      const lv_font_t * font, lv_color_t color);
```

```c
/* icons.c 追加（文件末尾） */
const lv_font_t * app_icon_font(void) { return &lv_font_icon_32; }  /* 名字以生成产物为准 */

/* 单码位 UTF-8 编码（0xE000~0xEFFF → 3 字节） */
lv_obj_t * icon_label(lv_obj_t * parent, uint32_t cp,
                      const lv_font_t * font, lv_color_t color)
{
    lv_obj_t * l = lv_label_create(parent);
    char utf8[5] = {0};
    utf8[0] = 0xE0 | ((cp >> 12) & 0x0F);
    utf8[1] = 0x80 | ((cp >> 6) & 0x3F);
    utf8[2] = 0x80 | (cp & 0x3F);
    lv_label_set_text(l, utf8);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* 不占 flex 位置，父容器用 pos/手动摆 */
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
```

注意：IGNORE_LAYOUT 依使用场景决定，rail 内若用 flex 摆放则不加该 flag——实现时 rail 用 flex（不 IGNORE），覆盖层定位场景才加。写成参数 `bool ignore_layout` 更稳。

- [ ] **Step 4: 构建验证**

Run: `cmake --build build_pc -j$(nproc) 2>&1 | grep -E "error|warning.*icon" | head; echo "exit=$?"`
Expected: 0 error（GLOB CONFIGURE_DEPENDS 自动纳入新 .c；若未触发重配置，touch 一下 CMakeLists.txt）

- [ ] **Step 5: Commit + 文档**

```bash
git add tools/gen_icon_font.py app/ui/fonts/lv_font_icon.c app/ui/fonts/lv_font_icon.h app/ui/icons.h app/ui/icons.c
git commit -m "ui-redesign: 引入图标字体（Lucide 子集 33 glyph）与 icon_label helper"
```

同步：《技术路线规约》§2.2 目录树 fonts/ 注明 `lv_font_icon.c/h`；§5 新增「§5.21 UI 图标字体契约」小节（码位私有区、生成脚本、33 个宏清单、`icon_label` 签名）。更新《开发进度.md》§0 标记。

---

### Task 2: 左 rail 导航替换底部 tab 栏

**Files:**
- Modify: `app/ui/ui.c`（重构 build_shell/build_tabbar→build_rail/switch_page/tab_index_of/TABS；删 TABBAR_BASE 相关）
- Modify: `app/ui/ui.h`（信息架构注释更新）

**Interfaces:**
- Consumes: Task 1 的 `icon_label` + `ICON_LP_*` 码位宏
- Produces: rail 7 项（i-home/i-user/i-list/i-face/i-wifi/i-cog/i-sys 对应 PAGE_HOME/USERS/LOGS/FACE/NETWORK/SETTINGS/SYSTEM），激活项左侧 accent 条 + 顶部留 logo 区
- Produces: `s_content` 宽度 = 屏宽 - RAIL_W；PAGE_KEYPAD/OTP 仍为全屏覆盖层（不占 rail 高亮；`rail_index_of(page)` 对它们返回 -1）

**布局规格（对齐预览 HTML）：**
- RAIL_BASE = 96（设计 px，×SY 缩放）；rail 背景 `st_panel` + 右侧 1px `TH_BORDER` 分隔
- rail 顶部（0~56px）：品牌区——大锁图标 `ICON_LP_LOCK`（accent 色）+ 下方小字「保险柜」
- rail 项：64×72 垂直 flex（icon 22px + label 11px），未选中 icon/label `TH_TEXT_MUT`，选中 `TH_ACCENT` + 左缘 3px accent 竖条（同现 tab 指示条机制）
- rail 底部：MQTT/WiFi 状态点从顶栏移到 rail 底（两个小 chip），顶栏右侧只留时钟 —— **若顶栏改动复杂则保留顶栏现状**，此项降级为「rail 底部放 WiFi/MQTT 小圆点」可选
- 内容区不再吃 TABBAR 高度；`s_content` height = 屏高 - topbar_h，x 起点 = rail_w

**关键：PAGE_SETTINGS 的入口语义变化。** 现有 SETTINGS 页是「三入口卡 + admin PIN 弹窗」（用户管理/网络/系统都从它进）。rail 直达 USERS/NETWORK/SYSTEM 后，这三个子页自身已带 admin PIN 验证逻辑吗？——查证：PIN 验证弹窗在 `page_settings.c show_verify()`，子页自身没有。**决策：rail 项 NETWORK/SYSTEM/USERS 点击 → 先走同一套 admin PIN 验证（把 show_verify 从 page_settings.c 抽成 `ui_admin_gate(ui_page_t target)` 放 ui_feedback 或新 ui_gate.c），通过后切页**。SETTINGS 页本身改为「主题选择 + 通用偏好」直达页（不再 PIN 门控，纯外观设置不敏感）。这一改动必须先回规约 §5。

- [ ] **Step 1: 抽取 admin PIN 门控为公共模块**

Create: `app/ui/ui_gate.c` / `app/ui/ui_gate.h`：

```c
/* ui_gate.h */
/* 敏感页（USERS/NETWORK/SYSTEM）的 admin PIN 门控：
 * rail 直达后验证弹窗不再属于 SETTINGS 页。验证通过 → ui_switch_page(target)。 */
void ui_admin_gate_show(ui_page_t target);   /* 弹 PIN 验证窗 */
bool ui_admin_gate_active(void);             /* 弹窗是否在屏（KEYPAD/OTP 切页时需让路） */
```

实现：把 `page_settings.c` 的 show_verify/close_verify/vkey_cb/vupdate_disp/vcancel_cb/vok_cb/vadmin_done + s_vpin/s_pending_page/s_overlay/s_vpin_disp/s_vmsg 整体搬到 ui_gate.c（逻辑零改动，只改回调里 `ui_switch_page(s_pending_page)` 不变）。page_settings.c 删掉这 200 行，ENTRIES 三卡改为纯展示或删页改主题页（见 Step 3）。

- [ ] **Step 2: 重写 ui.c 外壳为 rail 布局**

按上面布局规格改 `build_shell`（rail 左、content 右）、`build_tabbar`→`build_rail`（7 项 + 品牌区 + 底部状态点）、`tab_index_of`→`rail_index_of`、`TABS[3]`→`RAIL_ITEMS[7]`、`tabbar_refresh_theme`→`rail_refresh_theme`（同套 st_tab_hl 150ms 过渡机制照搬）。`RAIL_ITEMS` 定义：

```c
typedef struct { const char * label; uint32_t icon_cp; ui_page_t page; bool gated; } rail_def_t;
static const rail_def_t RAIL_ITEMS[7] = {
    { "主页",  ICON_LP_HOME,     PAGE_HOME,    false },
    { "用户",  ICON_LP_USER,     PAGE_USERS,   true  },   /* admin PIN 门控 */
    { "日志",  ICON_LP_LIST,     PAGE_LOGS,    false },
    { "人脸",  ICON_LP_FACE,     PAGE_FACE,    false },
    { "网络",  ICON_LP_WIFI,     PAGE_NETWORK, true  },
    { "设置",  ICON_LP_SETTINGS, PAGE_SETTINGS,false },
    { "系统",  ICON_LP_SYS,      PAGE_SYSTEM,  true  },
};
/* 点击：gated ? ui_admin_gate_show(page) : switch_page(page) */
```

`status_timer_cb` 里 s_tab_* 数组改为 s_rail_*[7]；switch_page 同步 rail CHECKED 态。FSM hook（WAIT_OTP→PAGE_OTP 等）不动。

- [ ] **Step 3: SETTINGS 页改为直达外观设置页**

`page_settings.c` 重写为：主题四选一卡（复用 SYSTEM 页现主题按钮组逻辑搬来或直接引导）+ 关于信息（版本/设备名/存储路径）。删 ENTRIES 三卡与 PIN 弹窗（已搬 ui_gate.c）。SYSTEM 页相应删主题按钮组（挪到 SETTINGS），SYSTEM 聚焦「安全策略 + 恢复出厂」单栏，右栏不再需要——布局从左右分栏改单栏，恢复出厂按钮整宽（修复折叠 bug 的第二重保险）。

- [ ] **Step 4: 构建修错 + 视觉验证**

Run: `cmake --build build_pc -j$(nproc) 2>&1 | grep -E "error" | head -20`
然后 `Xvfb :99 -screen 0 1024x600x24 & DISPLAY=:99 SAFE_TEST_SHOT=/tmp/rail_home.raw ./build_pc/bin/lvglsim`（截图转 PNG 后人工核对 rail 布局）

- [ ] **Step 5: 分层/测试检查 + Commit + 文档**

Run: `bash tools/check_layers.sh && ctest --test-dir build_pc --output-on-failure`（先 `pkill -f lvglsim`）

```bash
git add app/ui/ui.c app/ui/ui.h app/ui/ui_gate.c app/ui/ui_gate.h app/ui/pages/page_settings.c app/ui/pages/page_system.c app/platform/debug_hooks.c
git commit -m "ui-redesign: 左 rail 导航替换底部 tab 栏（7 入口 + admin PIN 门控抽公共）"
```

规约同步：§5 新增「§5.22 外壳导航结构」（rail 7 项表、gated 语义、ui_admin_gate 接口）；《开发进度.md》§0/§4。

---

### Task 3: 统一横向页面过渡（200ms ease）

**Files:**
- Modify: `app/ui/ui_anim.h`（改页面过渡常量：横向 + 时长）
- Modify: `app/ui/ui.c`（page_translate_y_cb → page_translate_x_cb；page_anim_out/in 改 x 方向 + 方向感知）

**Interfaces:**
- Consumes: 无新依赖
- Produces: `UI_ANIM_PAGE_IN_MS 200 / UI_ANIM_PAGE_OUT_MS 180 / UI_ANIM_TRANS_X_PX 40`（旧 Y 常量删）；`ui_page_transition_to` 带**方向**：新页 rail 序 > 旧页序 → 新页从右入（MOVE_LEFT 视感）、旧页左出；反之上右。全屏覆盖页（KEYPAD/OTP）用淡入淡出（不分方向）。

- [ ] **Step 1: 改 ui_anim.h 常量**

```c
/* ---------------- 页面切换过渡（v4 横向，重设计 spec §3）----------------
 * 方向感知：按 rail 序前进 → 新页从右滑入(+40px→0)+淡入，旧页左滑(-40px)+淡出；
 * 后退则镜像。全屏层（KEYPAD/OTP）无 rail 序 → 纵向 24px 保留（语义「浮起」）。 */
#define UI_ANIM_TRANS_X_PX      40      /* rail 页横向位移 */
#define UI_ANIM_TRANS_Y_PX      24      /* 全屏层纵向位移（保留旧值） */
#define UI_ANIM_PAGE_OUT_MS     180
#define UI_ANIM_PAGE_IN_MS      200
```

- [ ] **Step 2: ui.c 过渡改横向 + 方向**

`page_anim_out/in` 增加参数 `int dir`（+1 右入/-1 左入/0 纵向）；`ui_page_transition_to` 计算：两页都在 RAIL_ITEMS 且序号可比较 → dir = sign(new_idx - old_idx)；否则 dir=0 走旧纵向。回调改 `lv_obj_set_style_translate_x`。快速连点清理逻辑（lv_anim_delete 双 cb + page_reset）照旧。

- [ ] **Step 3: 构建验证 + 过渡中间帧实证**

Run: `cmake --build build_pc -j$(nproc) 2>&1 | grep error; DISPLAY=:99 SAFE_TEST_PAGE=LOGS SAFE_TEST_SHOT=/tmp/trans.raw SAFE_TEST_SHOT_MS=80 ./build_pc/bin/lvglsim`
（SAFE_TEST_PAGE 切页后 80ms 截图，抓过渡中间帧证明非瞬跳——debug_hooks 现成机制）

- [ ] **Step 4: Commit + 文档**

```bash
git add app/ui/ui_anim.h app/ui/ui.c
git commit -m "ui-redesign: 统一页面过渡（方向感知横向 200ms ease，全屏层保留纵向）"
```

规约 §5.22 补过渡参数表（常量/曲线/方向规则）。

---

### Task 4: HOME 仪表盘重设计（hero 卡 + 统计卡 + 事件列表）

**Files:**
- Modify: `app/ui/pages/page_monitor.c`（整页重排，业务逻辑 stats_worker/stats_done/monitor_timer_cb/is_unlocked_now 全保留）

**Interfaces:**
- Consumes: `icon_label`、`ICON_LP_*`
- Produces: 无新对外接口（page_monitor_create 签名不变）

**布局规格（对齐预览 §4）：**
1. **hero 卡**（全宽 ~140px 高，st_panel）：左侧大锁图标（`ICON_LP_LOCK` 48px，开锁时 `ICON_LP_LOCK_OPEN` + TH_OK，上锁 TH_DANGER——现有 monitor_refresh_state_colors 逻辑对接）；中间「保险柜已上锁」20 号 + 「最后开启：昨天 18:30」灰小字；右侧一键开锁大按钮（`st_accent_btn` + `ICON_LP_KEY` + 「开锁」→ PAGE_KEYPAD）
2. **4 统计卡**（一行 flex，各 flex_grow 1，~96px 高）：用户数（ICON_LP_USER）/今日事件（ICON_LP_LIST）/人脸模板（ICON_LP_FACE）/存储占用（ICON_LP_DATABASE）；左 icon 32px TH_ACCENT，右列「小灰标题 + 大数值」，数据沿用 stats_worker 异步（新增 face_count 统计：user_load_all 里数 face_id>=0；storage：store_dir() 下各文件 size 或简化为日志条数）
3. **最近事件列表**（flex_grow 1，卡内列表）：最多 6 行，每行 icon（按 evt 前缀映射 unlock→ICON_LP_KEY / user_*→ICON_LP_USER / setting→ICON_LP_SETTINGS / 其余 ICON_LP_INFO）+ 主文案（evt + user）+ 灰次文案 detail + 右侧时间；数据走 astore_query_log 取前 6 条

- [ ] **Step 1: 重写 page_monitor_create 布局**（按上述三段规格；全部用 flex，icon 全走 icon_label；旧圆环/双开锁卡/三联卡删除）
- [ ] **Step 2: stats_worker 扩两字段**

```c
typedef struct {
    int  user_count;
    int  event_count;
    int  face_count;        /* 新增 */
    char last_open[48];
    char storage_txt[24];   /* 新增，如 "2.1 MB" 或 "128 条" */
    bool net_connected;
} monitor_stats_t;
```

face_count 在 user_load_all 循环里 `if (us[i].face_id >= 0) fc++;`；storage_txt 用 `log_query(NULL,-1,...)` 的条数 snprintf（避免引入 stat() 依赖，保持简单——条数比字节数对用户更有意义）。

- [ ] **Step 3: 最近事件列表渲染**（logs_loaded 同款异步模式，取前 6 条渲染；evt→icon 映射函数 `static uint32_t evt_icon(const char * evt)`）
- [ ] **Step 4: 构建 + 截图验证 + Commit**

```bash
git add app/ui/pages/page_monitor.c
git commit -m "ui-redesign: HOME 仪表盘重设计（hero 卡 + 4 统计卡 + 最近事件列表）"
```

Run 截图：`DISPLAY=:99 SAFE_TEST_SHOT=/tmp/home.raw ./build_pc/bin/lvglsim`，转 PNG 核对三段布局。

---

### Task 5: USERS / LOGS 列表卡片化 + 图标化

**Files:**
- Modify: `app/ui/pages/page_users.c`（列表行重排；1615 行大文件，**只动 users_list_loaded 的行渲染 + 标题行，弹窗/worker/验证逻辑不动**）
- Modify: `app/ui/pages/page_logs.c`（时间线重排 + 图标 + 筛选 chips 圆角化）

**Interfaces:**
- Consumes: `icon_label`、`ICON_LP_*`、`safe_user_t`（role[16] "admin"/"user"/"temp"、valid_until、use_limit、used_count、face_id、enabled）

**USERS 行规格（预览 §4）：** 每行 64px：头像色块（40px 圆角方块，姓名首字，admin=TH_ACCENT 底反白字 / user=TH_ACCENT_20% 透明底 accent 字 / temp=TH_WARN 底反白字）+ 名称 17 号 + 灰副信息行（face_id>=0 时「已录人脸」+ created_at 截短，否则「PIN only」）+ 角色徽标 pill（管理员/普通/临时三色，temp 徽标旁加小字：valid_until 未来→「限时 · 余 N 天」、use_limit>0→「限次 · 余 M 次」、过期/用尽→灰「已过期」）+ 右侧操作按钮组（改密 ICON_LP_EDIT / 录人脸 ICON_LP_CAMERA / 删人脸 ICON_LP_CLOSE / 停用|启用文字 / 删除 ICON_LP_TRASH）保留现有回调与置灰逻辑。

**LOGS 行规格：** 每行 56px：左色条（4px，按结果 ok=TH_OK/fail=TH_DANGER）+ evt 图标（16px，evt_icon 复用 Task 4 同款映射，提到 ui 层共享：放 `app/ui/icons.h` 声明 `uint32_t ui_evt_icon(const char * evt);`，icons.c 实现，Task 4/5 共用）+ 主文案（evt + user）+ detail 灰字 + 右侧时间 ts。筛选 chips 改 SY(18) 圆角胶囊（st_ghost_btn + CHECKED 时 accent）已有，只调圆角字号。

- [ ] **Step 1: USERS 行渲染重写**（users_list_loaded 内的 for 循环体整体替换为新行结构；保留 admin_enabled 统计、last_admin 置灰、face_cap_ok 判断与全部事件回调接线）
- [ ] **Step 2: 临时用户标注**（副信息区按 valid_until/use_limit/used_count 三态显示；`hal_time()` 对比 valid_until）
- [ ] **Step 3: LOGS 时间线重写**（logs_loaded 行渲染替换；左色条 + 图标 + 两行文案）
- [ ] **Step 4: evt_icon 公共化**（Task 4 若已实现于 page_monitor.c 则挪到 icons.c 并两边改调用）
- [ ] **Step 5: 构建 + 截图验证（USERS/LOGS 两页 + add_user 弹窗）+ Commit**

```bash
git add app/ui/pages/page_users.c app/ui/pages/page_logs.c app/ui/icons.c app/ui/icons.h
git commit -m "ui-redesign: USERS/LOGS 列表卡片化（头像徽标时间线 + 图标化 + 临时用户标注）"
```

截图：`SAFE_TEST_PAGE=USERS` / `SAFE_TEST_PAGE=LOGS` / `SAFE_TEST_DLG=add_user` 各一张。

---

### Task 6: 其余页面卡片化（KEYPAD/NETWORK/FACE）+ 主题 token 微调

**Files:**
- Modify: `app/ui/pages/page_network.c`（表单项图标化 + 开关行样式）
- Modify: `app/ui/pages/page_face.c`（外框改圆角卡片、录入/删除入口重排；**视频预览与四态扫描框逻辑不动**——ui1 成果）
- Modify: `app/ui/pages/page_keypad.c` / `page_otp.c`（标题 + 显示框改卡片化外框，键盘逻辑不动）
- Modify: `app/ui/theme.c`（token 微调：TH_PANEL radius 16→18、阴影 opa 20→15、st_panel2 radius 12；rail 用新样式不需要新 token——若发现需要 surface-3 色则加 `TH_PANEL3`，**加 token 必须同时改四套主题 + 规约 §5**）

**Interfaces:**
- Consumes: `icon_label`、`ICON_LP_*`
- Produces: `st_card`？——**不新增**，复用 st_panel；只有四套主题都缺某个层次色时才加 TH_PANEL3（决策留给实现时视觉判断，加了必须四套同步 + 规约登记）

**page_network 图标化要点**（对齐预览「设置项带图标」）：每行开关项 = 左 32px 图标（WiFi/链路/上报）+ 行标题 + 右 switch（lv_switch 现有保留）+ 次行灰小字说明；卡片容器分组（「连接」「远程通道」两组 st_panel 包裹）。

**page_face 卡片化要点**：预览容器与扫描框外面包一层 st_panel 圆角卡（视频画布 320×240 不动，T3 帧率优化成果禁止回退）；录入/删除按钮换 ICON_LP_CAMERA / ICON_LP_CLOSE + 文字。返回按钮改 icon_label(ICON_LP_CHEV_LEFT)（FACE 从 rail 直达后「返回」语义改为回 HOME——rail 会高亮 FACE 项，返回按钮保留是习惯入口）。

- [ ] **Step 1: NETWORK 页卡片化**（分组卡 + 图标行；开关回调/落盘逻辑不动）
- [ ] **Step 2: FACE 页外框卡片化**（只动外层容器样式与按钮，视频链路零改动）
- [ ] **Step 3: KEYPAD/OTP 顶区卡片化**（标题/显示框外框；键盘不动）
- [ ] **Step 4: theme.c token 微调**（radius/阴影数值；跑 SAFE_TEST_THEME=0..4 五套截图确认无回归——注意 THEME_COUNT=5 含浅蓝）
- [ ] **Step 5: 构建 + 全页截图验证 + Commit**

```bash
git add app/ui/pages/page_network.c app/ui/pages/page_face.c app/ui/pages/page_keypad.c app/ui/pages/page_otp.c app/ui/theme.c app/ui/theme.h
git commit -m "ui-redesign: 其余页面卡片化 + 主题 token 微调（radius/阴影）"
```

---

### Task 7: 默认主题切月白 + 全量回归验收 + 文档收尾

**Files:**
- Modify: `app/ui/theme.c`（`g_idx = 4` → `g_idx = 1` 月白，一行）
- Modify: `app/platform/debug_hooks.c`（确认无需改——page_from_name 已全覆盖）
- Modify: `~/桌面/规约/技术路线规约.md`（§5.21/§5.22 若前序任务未同步则此处补齐）
- Modify: `~/桌面/规约/开发进度.md`（§0 标记 + §4 实测数据 + 里程碑）

**Interfaces:**
- Consumes: 全部前序任务
- Produces: 验收报告数据（截图清单、fps、过渡中间帧、线程数说明）

- [ ] **Step 1: 默认主题切月白**（theme_init 里 g_idx=1；注释同步）
- [ ] **Step 2: PC 全量验收**（对齐 spec §7 验收 1/2/3/4/5/7）：

```bash
pkill -f lvglsim; cmake --build build_pc -j$(nproc) && \
ctest --test-dir build_pc --output-on-failure && bash tools/check_layers.sh
# 8 页截图：HOME/USERS/LOGS/FACE/NETWORK/SETTINGS/SYSTEM/KEYPAD（SAFE_TEST_PAGE 逐个）
# 弹窗：SAFE_TEST_DLG=add_user / auth
# 过渡中间帧：SAFE_TEST_SHOT_MS=80
# 主题回归：SAFE_TEST_THEME=0..4 各一屏
# fps：SAFE_PERF_LOG=1 跑 30s 看 HOME 稳态
```

- [ ] **Step 3: 板子验证**（spec 验收 6/8/9，交叉编译 + 推固件 + dd 实屏截图 + 线程数 + 现场恢复）——**若板子此刻不可达，登记进《开发进度.md》为待办，不阻塞 PC 验收收尾**
- [ ] **Step 4: 文档收尾 + 最终 Commit**

```bash
git add <精确改动文件列表>
git commit -m "ui-redesign: 默认主题切月白 + 验收收尾"
```

《开发进度.md》更新：§0 标记「UI 重设计完成（PC 全量验收通过，板子验证 <日期/待办>）」、§4 填实测 fps/截图清单、里程碑一行「2026-09-12 UI 全面重设计：左 rail + 图标字体 + 横向过渡 + 卡片化（commits a1..a7）」。

---

## Self-Review 结论

1. **Spec 覆盖**：§2 图标策略→T1；§3 导航与过渡→T2/T3；§4 逐页要点→T4/T5/T6（FACE「保留现有预览+四态」明确不回退 T3 帧率优化）；§5 主题→T6/T7；§0.0 四裁定全覆盖（R9 明确不在本轮=解耦）；§6 步骤 1-7 与 T1-T7 一一对应；§7 验收 1-9 分布在 T2/T3/T7。SYSTEM 折叠 bug 在 T2（rail 第 7 项）+ T2 Step 3（SYSTEM 单栏化）双重修复。
2. **占位符**：T1 Step 1 的 SVG path 表标注「执行者补充完整表」——这是数据录入而非设计缺口，33 个 path 每个从 lucide.dev 复制，我已在脚本骨架里给了前两个示例格式。其余步骤无 TBD。
3. **类型一致性**：`icon_label(parent, cp, font, color)` 在 T1 定义、T2/T4/T5/T6 消费，签名一致；`ui_admin_gate_show(ui_page_t)` T2 定义使用一致；`ui_evt_icon` T4 定义 T5 共用（T5 Step 4 明确挪移步骤）；RAIL_ITEMS 的 7 页枚举与现有 `ui_page_t` 完全对齐（PAGE_FACE 现为全屏层，改 rail 后 tab→rail_index_of 对 KEYPAD/OTP 返回 -1 的语义已在 T2 Interfaces 写明）。
