# 单 helper 多 browser：现状与约定

2026-10-21 的产品决定：**一个 helper 进程承载多个 CEF browser**，同时支持标签和独立
窗口；架构只做 `linux64` / `linuxarm64` 两种 CEF 分发。

## 1. 已经能用的部分

- helper 内维护 `BrowserSlot` 注册表，键是 CEF 的 browser id；
- `new-browser` 的 `mode: "tab"` 把一个 `CefBrowserView` 加到共享 `CefWindow`，只有激活的
  view 可见；`mode: "window"` 创建独立 `CefWindow`；
- `select-browser` 切换激活 view、`g_browser`/`g_window` 和焦点；
- `close-browser` 先把 slot 里那份 `CefBrowserView` 引用丢掉（`RemoveChildView` +
  置空），再 `CloseBrowser`：窗口和 slot 都不是 view 的共有时，Alloy 才把这次关掉读成
  "关 browser" 而不是"关窗口"，`OnBeforeClose` 才会跑。关掉最后一个 browser 才结束 helper。
  **2026-10-22 曾实测只对 windowed 成立**：`close-browser 99` 回 `no-such-browser`
  （命令确实分发到了），`close-browser 2` 对一个 tab **什么都不会发生**（无
  `OnBeforeClose`、无 `browser-closed`），`close-browser 3` 对独立窗口则走完
  `browser 3 closing` → `browser 3 closed` → `browser 1 selected`。原因是 tab 窗口的
  `CefWindowDelegate` 自己留着第二份 view 引用（`helper/src/main.cpp`
  `HelperWindowDelegate::OnWindowCreated`），第一个 tab 因此永远关不掉——后面的 tab
  的 view 只被窗口和 slot 持有，反而正常。两处引用都清掉之后 tab 与 windowed 都走完整
  生命周期，`test/multi-browser-gate.sh` 断言的就是 `demo: browser 2 closed`；
  信号处理那一半的记录在 `docs/features/session-lifecycle.md` §7。见
  `docs/code-review-2026-10-22.md` M22 与 `docs/guides/minimal-host.md` §5。
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

窗口关闭时的交还也收口了：`nested_compositor_clear_input_surface(comp, owner)` 只在
当前 target 确实是 `owner` 时才把它清掉并立刻 `seat_retarget()`。这两条限定都是有原因的：
一个是**别人的窗口关掉不能抢走输入**（第二个窗口已经接手时，第一个窗口的 unmap 是空操作），
另一个是**交还发生在这一调用里，而不是等下一个事件**——在那之前指针和键盘会悬着一拍，
指向一个已经没有映射的 surface。`ChromiumView` 在 `unmap` 里为自己那一份调用它。

IME 侧不需要对应的改动：`text_input_retarget()` 挂在键盘焦点上，焦点挪到第二个窗口时第一个
的 text input 收 leave，`active_input` 自动换成新窗口的；第二个客户端在别人持有时发来的
`enable` 会被忽略。

## 自动门禁：`seat-selftest --second-window`

`test/seat-selftest.c` 的一个模式，`test/meson.build` 的 mode 列表里叫
`second-window`。它让**同一个 `shm-client` 进程**建两个 `xdg_toplevel`（app-id
`nested-shm-test` 与 `nested-shm-test-second`）——真实场景里两个 CEF 窗口本来就是同一个
helper 进程、同一条 `wl_client`。这个模式下两张图都不画测试花纹：

- **键盘焦点在哪边，哪边涂红（`c0 40 40`），另一边绿（`40 c0 40`）**；
- `wl_keyboard.enter/leave` 触发两张一起重绘；
- 收到一个键 Press 的那个窗口**自己把第二个 toplevel 销毁**——键只会送到 seat 认为持有
  键盘的那个 surface，所以"关闭"这一步是被路由驱动的，不是排在旁边。

脚本 17 步，其中 5 个检查点各做三层断言：

```
settle → 数到两个 toplevel（app-id 认，尺寸打印）
→ 指针+键盘进场 → set_input_surface(主)     → 断言键盘/指针焦点==主，主红副绿
→ clear_input_surface(副)（副不是 owner）    → 断言没有任何变化（守卫）
→ clear_input_surface(主)（主是 owner）      → 断言交还后落到 primary=副，副红主绿
→ set_input_surface(副)                      → 断言直接命名同样落到副
→ Esc → 副窗口自关 → 数到一个 toplevel      → 断言焦点==primary==主，主红
```

三层分别是：compositor 自己的 `keyboard_focus`/`pointer_focus`、`get_toplevels()` 的清单、
以及 `gdk_texture_download` 读到的**客户端画出来的像素**。像素那层不是装饰：按这个项目的
教训（`docs/internals/empty-request-slot.md` 那种"状态对、线上错"），只断 state 不断 wire 的门禁不算
门禁。任何一条不满足就 `second-window gate FAILED: …` 并以退出码 1 结束，和 drag-out /
drag-in 同款。整套跑完约 3 秒，不需要浏览器，headless 可跑。

两个方向的反向验证都做过：把 `clear_input_surface` 的 owner 守卫拆掉，第三条断言立刻红
（键盘和指针都跑到 primary，两张图跟着翻转）；把里面的 `seat_retarget()` 拆掉，交还那一跳
红。脚本里"非 owner 交还"这一步特意排在**主窗口持有输入、而 primary 是副窗口**的时刻——
否则清空与不清空的结果都是副窗口，那条断言就什么也抓不到。

## 仍然存在的边界

- **在独立窗口里发起拖拽**：出界移交的 `GdkDrag` 起在 demo 主窗口的 native surface 上
  （`docs/features/drag-loop.md` §6），从独立 host 窗口里发起的拖拽会挂错窗口。罕见路径，这一轮
  不做，先记在这里。
- `linuxarm64` 只有 fetch/install 路径，没有目标机实跑。


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

其他命令/事件的可选 `browser` 字段见 `docs/guides/control-channel.md`。缺省行为是当前激活
browser，避免把已有单 browser 调用全部改坏。
