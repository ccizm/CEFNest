# 页面全屏：Alloy 的契约与接线

日期：2026-10-20
功能：页面里的 `requestFullscreen()` / 视频全屏按钮，现在会让**宿主窗口**
真正进入全屏，页面随窗口重排到全屏尺寸；页面退出时窗口退出。

## 1. 根因：Alloy 的设计契约，不是 bug

CEF 154 的 Alloy runtime 在页面切换全屏模式时**只通知、不执行**：
`CefDisplayHandler::OnFullscreenModeChange(browser, fullscreen)` 的文档原文是
"the client is responsible for triggering the fullscreen transition (for
example, by calling CefWindow::SetFullscreen when using Views)"。Chrome runtime
才自动触发。helper 此前没有实现这个回调，所以页面进全屏后平台窗口纹丝不动
（页面状态 FS-ON、`innerWidth` 不变——状态与尺寸脱钩）。

## 2. 接线（helper/src/main.cpp）

- `HelperClient::OnFullscreenModeChange` → `g_window->SetFullscreen(fullscreen)`
  （views → aura → ozone → `xdg_toplevel.set_fullscreen` 到嵌套合成器）；
- `HelperWindowDelegate::OnWindowFullscreenTransition(completed)`：窗口因
  **用户动作**（WM 快捷键/标题栏）退出全屏时，Alloy 不会退出 browser
  fullscreen——按 `CefBrowserHost::ExitFullscreen` 文档的要求，这里补调
  `ExitFullscreen(true)` 让页面跟随；
- **Esc 语义分级**：全屏中 Esc = 退出页面全屏（`ExitFullscreen` +
  `SetFullscreen(false)`），不关闭浏览器；非全屏中 Esc 仍是关闭（探针惯例）。

## 3. 合成器侧（nested-xdg.c / nested.c）

- `xdg_toplevel.set_fullscreen/unset_fullscreen` 不再是空函数：交给
  `nested_request_window_fullscreen(surface, enter)`；
- 门控：只有**正在显示的 toplevel**（`surface_is_painted`）可以提请求——隐藏
  toplevel 与 popup 无权移动用户的窗口；
- 合成器发 `::window-fullscreen-request(enter)` 信号给控件；
- 控件把 root 窗口全屏/还原，并经
  `nested_compositor_set_window_fullscreen()` 把窗口**实际**状态回报——
  configure 的 states 数组按实际状态广告 `FULLSCREEN`（与 MAXIMIZED 互斥）。

## 4. 验证

- selftest：`shm-client --fullscreen` 在 30/150 帧发两个边缘请求，
  `seat-selftest` 连接信号断言 ENTER/EXIT 都被看到（meson test 新增
  `seat-selftest-fullscreen`，且 runner 对该门加 6s 宽限等待防慢机 flake）；
- 真 CEF 端到端：`fs.session` 点按钮 → `FS-ON innerWidth=1536`（全屏 CSS
  尺寸），dmabuf 长到全屏物理尺寸；`document.exitFullscreen()` → `FS-OFF`
  + unset 请求到达合成器。

## 5. 已知遗留

- **退出后的窗口尺寸恢复**：页面退出全屏、`unset_fullscreen` 到达后，宿主
  窗口被 GTK 还原的尺寸出现了停留在 1920×1080（≈全屏 CSS 尺寸×1.25）的
  观测，未回到进入前的 1000×653。页面级状态正确，窗口可移动/缩放，但尺寸
  还原路径需要专项排查（怀疑全屏期间 `set_size` 的推送与 GTK 的还原序列
  竞争）；期间的 `js` 读回可能出现 `{}`（退出过渡与 V8 注入读回的竞态）。
