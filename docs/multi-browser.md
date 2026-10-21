# 单 helper 多 browser：现状与约定

2026-10-21 的产品决定：**一个 helper 进程承载多个 CEF browser**，同时支持标签和独立
窗口；架构只做 `linux64` / `linuxarm64` 两种 CEF 分发。

## 1. 已经能用的部分

- helper 内维护 `BrowserSlot` 注册表，键是 CEF 的 browser id；
- `new-browser` 的 `mode: "tab"` 把一个 `CefBrowserView` 加到共享 `CefWindow`，只有激活的
  view 可见；`mode: "window"` 创建独立 `CefWindow`；
- `select-browser` 切换激活 view、`g_browser`/`g_window` 和焦点；
- `close-browser` 先 `RemoveChildView` 再 `CloseBrowser`，避免 CEF 把整个 tab 窗口一起
  销毁；关掉最后一个 browser 才结束 helper；
- demo 有标签条（`+` 新标签、`▣` 新窗口）；标签按钮调用 session API；
- 所有页面事件带当前激活 browser 的 `browser` id；新增 `browser-created` /
  `browser-selected` / `browser-closed`；
- 单 browser 的旧路径保持：不带 `browser` 的命令仍作用于激活 browser。

实测：两个 tab 切换时 dump 分别是 MENU-PAGE 和 spellcheck 页面；`new-window` 创建了第二个
`CefWindow`，host 的同一个嵌套 view 会跟随当前合成器 primary。

## 2. 独立 host 窗口

已经落地：

- `NestedCompositor::toplevels-changed` + `nested_compositor_get_toplevels()`；
- `nested_compositor_get_paint_stack_for()` 允许一个 view 画指定 `NestedSurface`，而不是
  compositor 的 primary；
- `ChromiumView` 新增 `chromium_view_new_shared()` 和
  `chromium_view_set_target_surface()`；
- demo 在 `windowed` 的 `browser-created` 上开第二个 `GtkWindow`，用共享 compositor 的
  view 绑定该 CEF window 的 surface；
- 主 tab 窗口固定绑定 tab CEF window 的 surface，独立窗口不再抢主 view；
- `::open-url`（页面 `window.open`、菜单"在新窗口打开"）不再另起 demo 进程，而是
  `chromium_session_new_browser(url, windowed=TRUE)` 走同一个 helper，随后的
  `browser-created` 自动给它开 GTK 窗口；独立的 `open_url_in_new_window()` 进程路径
  只留作没有活跃 session 时的兜底；
- 实测：`new-window` 后主窗口仍显示 MENU-PAGE，第二个 GTK 窗口显示 spellcheck 页面；
  `select-browser 1` 切回 tab 后主窗口内容保持正确。

输入也做了第一层：`nested_compositor_set_input_surface()` 让获得焦点/指针进入的
`ChromiumView` 指定自己的 `NestedSurface`；pointer/keyboard 不再只跟随 compositor 的
primary。`chromium_view_new_shared()` 的 view 在 motion/button/focus 时设置该 target。

pointer 的 popup 命中也已用 target 自己的 paint stack 做 `surface_at`，所以独立窗口里的
右键菜单/`<select>` 弹层能落到 target 的 popup 上，而不是主窗口。

**仍待确认**：多窗口同时输入/焦点切换只有手工观察，没有像菜单那样的自动门禁；
`linuxarm64` 也只有 fetch/install 路径，没有目标机实跑。文档把这两点列成验证缺口，不把
它们说成已验证。

## 3. 协议

```json
{"cmd":"new-browser","url":"https://…","mode":"tab"}
{"cmd":"select-browser","browser":7}
{"cmd":"close-browser","browser":7}
```

事件：

```json
{"event":"browser-created","browser":7,"url":"…","windowed":false}
{"event":"browser-selected","browser":7,"windowed":false}
{"event":"browser-closed","browser":7}
```

其他命令/事件的可选 `browser` 字段见 `docs/control-channel.md`。缺省行为是当前激活
browser，避免把已有单 browser 调用全部改坏。
