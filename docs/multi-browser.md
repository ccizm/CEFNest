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

## 2. 还没完成：独立 host 窗口

`new-window` 在 helper 里确实是独立 `CefWindow`，但宿主目前还是一个 `ChromiumView`：
一个嵌套 compositor 只把一个 primary surface 画到 widget 里。所以“独立窗口”现在表现为
helper 的第二个 CEF toplevel 抢到同一个 host view，而不是第二个 GTK 窗口。

要真正收口需要：

1. `ChromiumView` 支持共享同一个 `NestedCompositor`，并绑定到指定 `NestedSurface`；
2. `NestedCompositor` 暴露 toplevel 列表/选择，把 host 窗口和 CEF 窗口对上；
3. 每个 host 窗口有自己的输入焦点和 `ChromiumView`，但不能各自复制一份 compositor。

在此之前，标签形态是可用的；`new-window` 只是 helper/protocol 层的窗口。

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
