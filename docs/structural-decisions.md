# 结构性决策：多标签/多窗口与架构分发

这两项在 2026-10-21 的优先级里被标成“等用户拍板”。这里只记录已具备的条件和需要做的
选择，不把任何产品形态偷偷写进代码。

## 1. 单 helper 多 browser 路由

**决定（2026-10-21）**：单 helper、多 browser；标签和独立窗口都要支持。当前实现先落地
helper 注册表、标签条、`new-window` 的 CEF toplevel，以及共享 compositor / 指定
surface 的独立 host 窗口，见 `docs/features/multi-browser.md`。

现状：一个 helper 进程创建一个 `CefBrowserView`/一个 browser；控制 socket 的命令和
事件没有 browser 标识，`ChromiumSession` 也是一对一。

要做多标签，最低成本的两条路：

| 方案 | 改动 | 风险 |
| --- | --- | --- |
| 一个 helper 内多个 browser（标签） | helper 为每个 browser 分配 `browser_id`，所有命令/事件带 id；host 为每个 id 建一个 view/surface | 事件路由、下载/菜单/权限的归属、焦点/键盘目标要全部带 id |
| 每个标签一个 helper（多进程） | host 起多个 session，各自一个 view | 内存/进程数上升，标签间共享登录/缓存要另外做；实现风险低 |

**必须先决定**：标签是“一个窗口里切换页面”还是“每个页面一个顶层窗口”；关闭窗口是关
browser 还是关整个 helper；右键菜单/下载/权限弹窗挂在哪个标签上。

当前代码可以复用 per-browser 的 CEF 回调，但 **protocol 的 browser 路由是新增的**，不是
把现有单 browser 改成多 browser 的无痛开关。

## 2. 多 toplevel 显示策略

`NestedCompositor` 现在只有一个 primary surface；一个页面的多个顶层窗口可以作为多个
`NestedSurface` 叠放，但哪些算“窗口”、哪些算“标签”、窗口关闭/最小化/全屏状态如何
回到 CEF，是宿主产品决策。现有 `nested_compositor_set_window_fullscreen` 只服务一个
window，不能直接扩成多 window 而保持语义。

## 3. loong64 / aarch64 供给

`scripts/fetch-cef.sh` 目前硬编码：

```text
linux64_minimal
```

**决定（2026-10-21）**：只支持 `linux64`（amd64）和 `linuxarm64`（aarch64）。
`scripts/fetch-cef.sh` 已按 `CEF_ARCH` 选择这两个 CEF minimal 包；但本机只有 x86_64，
arm64 仍需目标机验证。要收口需要：

1. 找到对应架构、对应 CEF 154 的官方 minimal 包，或决定自建；
2. 在目标架构上实际跑 `scripts/fetch-cef.sh`、helper 构建、`meson test` 和至少一条真
   Chromium 会话；
3. 只有三步都过，才把架构从“安装侧预留”改成“已验证”。

## 4. 结论

- **多标签/多窗口**：需要产品形态和 browser 路由的明确选择；在此之前不改协议。
- **loong64/aarch64**：需要真实分发包和目标机器，不在 x86_64 机器上凭空支持。
