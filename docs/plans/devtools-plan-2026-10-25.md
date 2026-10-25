# CEFNest 开发者工具方案（参数开关 + 独立 GTK 窗口，2026-10-25）

裁定已定：**不做停靠**（remote_debugging_port 路线整体不采用）、**不做插件**
（不引入插件机制）、**做参数开关**（组件属性一个布尔 + demo 一个命令行参数）、
**DevTools 以独立 GTK 新窗口形式打开**（走现有窗口化浏览器管线）。程序化
open/close API 不在本版范围。本文档为可直接执行的落地版本，改动点标注到
已核实的 `文件:行号`。

---

## 〇、机制事实（2026-10-25 已核实）

**CEF 侧**

- `third_party/cef/dist/include/cef_browser.h:582-598`：
  `ShowDevTools(windowInfo, client, settings, inspect_element_at)` /
  `CloseDevTools()` / `HasDevTools()`。DevTools 以**独立但关联的浏览器**打开
  （目标浏览器关闭时 CEF 自动关闭它）；已打开时再次调用只是聚焦。
- 目标浏览器包在 `CefBrowserView` 里时 `windowInfo` 被忽略——helper 全部浏览器
  都是 BrowserView 包裹，因此 DevTools 必然由 CEF 自建窗口，落在与现有独立
  窗口完全相同的 ozone/Wayland 渲染路径上。

**Helper 侧（helper/src/main.cpp）**

- `2250-2285` `OnAfterCreated`：slot 只为 helper 自建浏览器预注册；CEF 内建
  浏览器（即 DevTools）无 slot，`browser-created` 事件仍会发出但
  `windowed=false`（2284）——**这是唯一需要修的缺口**。
- DevTools 浏览器的识别依据：主框架 URL 以 `devtools://` 开头。
- `2359` `OnBeforeClose`：对**所有**浏览器（含无 slot 的）发 `browser-closed`
  并清理 slot 表——DevTools 的关闭事件天然到达宿主。
- `1945` `HandleCommand` 的 `"a11y"` 分支：运行时开关命令的现成模板。
- `1128-1160` `OnBeforeContextMenu`：自建菜单行（`kMenu*` 26501–26507）的
  插入点；`1203-1253` `OnContextMenuCommand`：命令分发（`MENU_ID_VIEW_SOURCE`
  拦截，1242，为样板）。
- `2492-2530` `OnPreKeyEvent`：已存在，现处理 Esc（退出全屏）与 Ctrl+Q——
  F12 / Ctrl+Shift+I 快捷键的落点。
- `2690-2710` `close-browser` 命令：`windowed=true` 分支不触碰 view，直接
  `CloseBrowser(true)`——对无 view 的 DevTools slot 天然安全。

**宿主侧**

- `src/main.c:50-53`：demo 维护 `browser_windows`（id→GtkWindow*）、
  `browser_views`（id→ChromiumView*）、`assigned_surfaces`、
  `pending_window_ids`。
- `src/main.c:423` `open_browser_window()`：为窗口化浏览器建**真 GtkWindow**
  + `chromium_view_new_shared(compositor)`（共享同一合成器的第二个 view）。
- `src/main.c:289` `assign_browser_surfaces()`：按 **FIFO** 把合成器上的
  toplevel 认领给等待中的窗口（`chromium_view_set_target_surface`），不依赖
  标题——DevTools 的 toplevel 无需任何特殊标记。
- `src/main.c:196-218` `session_browser_created` → `windowed` 时调
  `open_browser_window(id)`；`~246` `session_browser_closed` → 窗口清理。
- 开关传播链模板：`session_send_a11y`（chromium-session.c:200，调用点
  236/539/2945）→ `{"cmd":"a11y","enabled":…}` → helper 1945。

**资源事实（"能否剔除"的答案）**

- DevTools 协议 agent 编在 `libcef.so`，前端资源打包在 `resources.pak`
  （24 MB，实测含 `kDevTools` 资源项）。**不可从预编译 CEF 发行版剔除**
  （剔 = 源码重建 Chromium/CEF，小时级构建、无受支持开关、体积收益 MB 级）。
- 产品语义上的"剔除"已成立且方案保持：默认状态**不可达**——从不调用
  ShowDevTools、无菜单行、`remote_debugging_port` 不设置（CEF 默认 0=关闭）。

---

## 一、行为矩阵

| 开关 | 入口 | 行为 |
| --- | --- | --- |
| 关（默认） | 无 | 无"检查"菜单行、F12 无效；包内 DevTools 不可达（与现状一致） |
| 开 | 右键"检查" / F12 / Ctrl+Shift+I | helper 调 `ShowDevTools` → `browser-created(windowed=true, devtools=true)` → 宿主按现有管线开 **GTK 新窗口**（标题 "DevTools — <页面标题>"） |

关闭路径双向：用户关 DevTools 的 GTK 窗口 → `close-browser(devtools id)` →
`CloseBrowser`；用户关目标页面 → CEF 关联语义自动带走 DevTools → 两条
`browser-closed` 先后到达宿主。

---

## 二、设计

### 2.1 开关传播链（复刻 a11y）

- **session**：属性 `"devtools-enabled"`（gboolean，默认 FALSE，READWRITE）+
  `chromium_session_set_devtools_enabled()` / `get_…()`。设置时若 session
  正在运行，立即发 `{"cmd":"devtools","enabled":bool}`；否则在 start() 就绪
  后随 a11y 初值一起发（`session_send_a11y` 三个调用点并列新增
  `send_devtools_state()`）。
- **helper**：`HandleCommand` 增加 `"devtools"` 分支 → 置
  `g_devtools_enabled`（与 a11y 的 1945 行并列）。

### 2.2 入口（全部 gated on `g_devtools_enabled`）

1. **右键菜单**：在 `RunContextMenu` 重建模型时（自建链接/图片行之后），
   用 `model->AddSeparator()` + `model->AddItem(kMenuInspect = 26508, "检查")`
   把这一行放到**菜单最下面**——与 Chrome 一致，2026-10-25 按用户要求从
   "自建行末尾"（即菜单顶部）改为模型末尾；`OnContextMenuCommand` 新增 case →
   `ShowDevToolsFor(browser, {params->GetXCoord(), params->GetYCoord()})`，
   返回 true（右键即检查，与 Chrome 一致）。
2. **键盘**：`OnPreKeyEvent` 增加 F12（windows_key_code 0x7B）与
   Ctrl+Shift+I 分支 → 对事件所在的 browser 调 `ShowDevTools`（不带坐标），
   `*is_keyboard_shortcut = true`。

### 2.3 DevTools 浏览器收编（核心缺口修复）

`OnAfterCreated`（2250-2285）：当 `slot == NULL` 且主框架 URL 以
`devtools://` 开头时，建 slot：

```
entry.slot = entry.browser id; entry.windowed = true; entry.devtools = true;
entry.view = nullptr; entry.window = nullptr;   // CEF 自建窗口，不经 helper
```

并让 `browser-created` 事件带 `windowed=true, devtools=true`。配套影响点
（均有现成判空，实施时以回归确认）：

- `SelectBrowserById` 对 `view=null` 的 slot：`2270-2271` 与 195 行已有判空；
  DevTools 不是请求 slot，不抢选（2268 `!g_browser || select_this` 均不成立）。
- `close-browser`：windowed 分支跳过 view-detach，直接 `CloseBrowser(true)`。
- `browser-closed`：`OnBeforeClose` 无条件发出（2366-2369）。

slot 结构体（137 行一带）加 `bool devtools = false;` 字段。

### 2.4 宿主（src/chromium-session.c/.h）

- `::browser-created` 信号追加尾参 `gboolean devtools`（0.0.1 阶段无 ABI
  负担），emit 处与 `handle_browser_created` 同步。
- 新 API 仅开关两个：`chromium_session_set_devtools_enabled()` /
  `chromium_session_get_devtools_enabled()`。**不加** show/close_devtools
  （裁定：参数开关语义保持最小；将来要加时控制通道命令已就位）。
- 头文件注释写明安全语义：开启即把页面调试权（任意 JS、任意存储读取）交给
  本机使用者；默认关闭。

### 2.5 demo / examples

- `src/main.c`：新增 `--devtools` 命令行参数 → 属性置 TRUE；
  `session_browser_created` 增加 devtools 参数 → `open_browser_window(id)`
  照常调用，GTK 窗口标题改为
  `devtools ? "DevTools — <title-or-url>" : "CEFNest window"`。
- 新增 `examples/devtools.c`（以 browser.c 为模板）：开开关、提示
  "右键 → 检查 或 F12"、演示 devtools 窗口与目标页面的双向关闭。

### 2.6 明确不做

程序化 open/close API、停靠（remote_debugging_port 及其安全面）、插件机制、
宿主自定义菜单项注册。

---

## 三、改动清单

| 文件 | 改动 | 锚点 |
| --- | --- | --- |
| helper/src/main.cpp | `g_devtools_enabled` + `"devtools"` 命令分支 | 1945 旁 |
| | slot 结构体 `bool devtools` | 137 |
| | `kMenuInspect = 26508` 常量 | 26501–26507 旁 |
| | RunContextMenu 菜单行（分隔线 + "检查"，追加到模型末尾） | 1128–1160 |
| | OnContextMenuCommand `case kMenuInspect` | 1242 旁 |
| | OnPreKeyEvent F12 / Ctrl+Shift+I | 2492–2530 |
| | OnAfterCreated devtools slot 收编 + 事件字段 | 2250–2285 |
| src/chromium-session.h | `set/get_devtools_enabled` + 信号文档（更新 browser-created 段） | 头文件 |
| src/chromium-session.c | 字段/属性/pspec/get-set_property | 80、3316、3507–3510 |
| | `send_devtools_state()` + 三个调用点 | 200、236、539、2945 |
| | `::browser-created` 加参 + emit | 信号定义与 handle |
| src/main.c | `--devtools`、回调签名、窗口标题 | 196–218、423 |
| examples/devtools.c | 新增 | — |
| test/devtools-gate.sh | 新增 | — |
| README.md / .zh-CN.md | "开发者工具"小节（开关用法 + 默认关闭说明） | 文档 |
| docs/plans/ | 本文件 | — |

---

## 四、测试门禁（test/devtools-gate.sh，沿用 dnd-gate.sh 驱动 demo 的模式）

1. **默认关**：右键菜单 JSON 无 id 26508 行；无第二个窗口出现。
2. **开关开**（`--devtools`）：菜单行出现；`CHROMIUM_SESSION_SCRIPT` 的
   `menu-pick 26508` → helper 日志 `browser created … windowed=1`、
   `browser-created` 带 `devtools=true` → demo 输出 standalone window
   （第二 GTK 窗口拿到 surface）。
3. **关闭 DevTools 窗口**：`browser-closed(devtools id)` 到达，demo 窗口清理。
4. **关闭目标页面**：DevTools 的 `browser-closed` 跟随到达（CEF 关联语义）。
5. **中途关开关**：下一个右键菜单无 26508 行。
6. **回归**：multi-browser-gate.sh（窗口化路径共用）、ci-all.sh 全量。

---

## 五、实施顺序（每步可编译、可回归）

| 步 | 内容 | 完成标志 |
| --- | --- | --- |
| 1 | helper：命令分支、菜单行、快捷键、slot 收编 | 编译过；手动 --auto-quit 冒烟 |
| 2 | session：属性/开关命令/信号加参 | 门禁 1 过 |
| 3 | demo --devtools + examples/devtools.c | 门禁 2 过 |
| 4 | devtools-gate.sh 全部断言 | 门禁 1–5 全绿 |
| 5 | README 双语 + 回归 multi-browser + ci-all | 全绿 |

---

## 六、风险与验证点（不改变设计）

- **已实测落地（2026-10-25）**：CEF 自建窗口的框无法事后去除——事后把
  popup view 挪进自建窗口两条路都死（首帧之前挪：渲染管线坏死、永不映射；
  首帧之后挪：StatusBubble 的 WaylandBubble 段错误），且自建 Chrome 风格
  窗口承载新 view 也会在 StatusBubble 崩溃。最终走
  `GetDelegateForPopupBrowserView` + `OnPopupBrowserViewCreated` 钩子：popup
  view 由我们的 Chrome 风格 delegate 接管（满足 CEF 的 style CHECK），壳
  窗口用我们的无框 delegate 建在 CEF 默认窗口之前——devtools-gate 44 项
  断言全绿，截图确认无 CEF 框。白内容注意事项：devtools 前端经
  remoteBase（appspot）拉模块，离线环境白屏属网络问题，与窗口无关。
- FIFO 匹配依赖 `browser-created` 先于 toplevel 到达：多浏览器窗口已保证
  此顺序，DevTools 沿用同一时序。
- `SelectBrowserById` 对 `view=null` slot 的容错：195/2270 行已有判空，
  multi-browser-gate 回归确认。
- 安全边界：开启开关 = 本机使用者获得页面完全调试权；`remote_debugging_port`
  在任何路径下都不设置。
