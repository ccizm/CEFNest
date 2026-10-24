# GTK4 ↔ Chromium 统一键盘焦点链：设计提案

场景：同一个窗口里有 GTK 原生控件和 `ChromiumView`，例如
`按钮1 · 按钮2 · [Chromium: html按钮1, html按钮2] · 按钮3`。
期望 Tab 的完整顺序是 `按钮1 → 按钮2 → html按钮1 → html按钮2 → 按钮3 →（回绕）`，
实际是两个闭环：GTK 控件之间切换、网页内部切换，边界上互相过不去。

## 根因

两侧各有一条闭合的焦点环，缺的是边界上的两个"出口/入口"：

1. **GTK → 网页方向其实是通的**：`ChromiumView` 是 focusable
   （`src/chromium-view.c:930`），GTK 的 Tab 焦点遍历走到 view 时，view 抓住
   焦点，此后按键经 `nested_compositor_notify_keyboard_key()` 送给 Chromium。
2. **网页 → GTK 方向断了**，有两处原因：
   - view 的 key 控制器对每个键都 `return TRUE`（`src/chromium-view.c:817,835`），
     Tab 永远不会冒泡回 GtkWindow，GTK 无从发起 move-focus；
   - Chromium 在 Tab 到达页面边缘（最后一个元素再 Tab / 第一个元素再
     Shift+Tab，或页面无可聚焦元素）时，会通过 TakeFocus 机制告诉"宿主浏览器"
     "焦点应该离开网页"。CEF 把它暴露为 `CefFocusHandler::OnTakeFocus()`，
     而 helper 虽然实现了 `CefFocusHandler`，却只写了 `OnGotFocus`
     （`helper/src/main.cpp:2023`），**`OnTakeFocus` 未实现，该事件被默认实现
     静默丢弃**——这就是边界无法跨越的直接原因。

CEF 对 `OnTakeFocus` 的文档原文即此场景：
*"Called when the browser component is about to lose focus. For instance, if
focus was on the last HTML element and the user pressed the TAB key."*

## 方案总览

焦点链上把 `ChromiumView` 当作一个"内部还有焦点的容器"对待：

```
            GTK 焦点链（GtkWindow move-focus 遍历）
  按钮1 ──▶ 按钮2 ──▶ ChromiumView ──▶ 按钮3
                        │  ▲
        入向：focus vfunc│  │出向：OnTakeFocus
        抓焦点+合成一次  │  │"take-focus"事件→
        Tab/Shift+Tab   │  │emit "move-focus" 走 GTK 链
                        ▼  │
                 Blink 页内 Tab 遍历
               html按钮1 ⇄ html按钮2
```

- **出向（网页 → GTK）**：helper 实现 `OnTakeFocus`，经控制通道发
  `{"event":"take-focus","browser":<id>,"next":true|false}`；
  `ChromiumSession::handle_event()` 收到后调用 view 的新方法
  `chromium_view_release_chain_focus(view, forward)`，view 在自己身上
  `g_signal_emit_by_name (view, "move-focus", GTK_DIR_TAB_FORWARD/BACKWARD)`。
  `move-focus` 冒泡到 GtkWindow 的 class handler 后，GTK 会从 view 在焦点链中
  的位置继续找下一个控件（这与 GtkTextView 让出焦点是同一机制）。焦点落到
  按钮3，view 的 `has-focus` 变 FALSE，既有逻辑 `keyboard_active(FALSE)` 让
  Chromium 收到 `wl_keyboard.leave`，页面失焦——与原生应用行为一致。
- **入向（GTK → 网页）**：view 覆写 `GtkWidgetClass::focus` vfunc
  （焦点链走到 view 时被调用，带方向）：
  - session 未运行 / 无 target：返回 `FALSE`，焦点链把 view 跳过
    （等价于"没有可聚焦子项的容器"）；
  - 正常：`gtk_widget_grab_focus()` 自己，然后按进入方向请求一次
    **入页推进**——经控制通道发 `{"cmd":"focus-advance","forward":bool}`，
    helper 用 `CefBrowserHost::SendKeyEvent()` 向页面合成一次
    Tab / Shift+Tab 键。Blink 对"文档有焦点但无焦点元素"的 Tab/Shift+Tab
    语义就是聚焦第一个 / 最后一个可聚焦元素，于是 `按钮2 --Tab--> html按钮1`
    一步直达（Shift+Tab 从按钮3 进来则直达 html按钮2）。
  - 用合成 Tab 而不是 JS 查询 first/last 可聚焦元素，是因为它走 Blink 原生
    遍历，shadow DOM、`tabindex`、radio group、动态 DOM 全部天然正确。
- **点击进入不受影响**：鼠标点击走 `gtk_widget_grab_focus()`
  （`src/chromium-view.c:572`），不经过 `focus` vfunc，不触发入页推进，
  页面自然聚焦点击处的元素。
- **防回环 guard**：view 自己发出 `move-focus`（出向）后紧接着的一次
  `focus` vfunc 调用要直接拒绝（返回 `FALSE`）。否则会出现死循环：空页面
  Tab → 合成 Tab → 无可聚焦元素 → 立即 `take-focus` → move-focus → 焦点又
  落回 view → 再合成 Tab → ……。带上 guard 之后，空页面在 Tab 遍历中被
  跳过（GTK 语义上"无焦点的容器"正是被跳过），点击仍可进入。
  guard 在焦点确定落到别的控件或回到主循环后清除。

## 改动清单

| 文件 | 改动 |
| --- | --- |
| `helper/src/main.cpp` | `HelperClient` 实现 `OnTakeFocus(browser, next)`：`Emit` 新事件 `take-focus`（带 `browser` id 与 `next`）；`HandleCommand()` 新增 `focus-advance {"forward":bool}`：对当前 browser 的 host 调 `SendKeyEvent()`，构造 `CefKeyEvent{KEYEVENT_RAWKEYDOWN/KEYUP, windows_key_code=VK_TAB(9), modifiers=±EVENTFLAG_SHIFT_DOWN}`（`HandleCommand` 本已在 CEF UI 线程执行）。 |
| `src/chromium-session.c` | `handle_event()` 处理 `take-focus`：与当前选中的 browser id 比对（session 已跟踪 `browser-selected`），不符则忽略，相符则调 `chromium_view_release_chain_focus()`；新增内部函数发送 `focus-advance` 命令，`set_view()` 时连接 view 的新信号。 |
| `src/chromium-view.c/.h` | 覆写 `focus` vfunc（拒绝/抓取+入页推进/guard）；新增公开方法 `chromium_view_release_chain_focus(view, forward)`（设 guard、emit `move-focus`）；新增信号 `::focus-advance-requested(forward)` 供 session 转发为 `focus-advance` 命令。 |
| `docs/control-channel.md`（及 zh-CN） | 补两个消息的协议说明。 |
| `examples/` | 新增（或扩展 toolbar 示例）：`按钮 · view · 按钮` 布局 + `data:` 测试页（两个 html 按钮），作为验收用例。 |

## 分阶段落地

- **Phase 0 · 验证（半天）**：在 `OnTakeFocus` 里临时 `fprintf`，用 154 版
  CEF、Alloy 窗口模式确认"最后元素再 Tab / 首元素再 Shift+Tab"确实触发回调。
  触发不了再启用备选方案（见下）。这一步必须先做，它是整个方案的地基。
- **Phase 1 · 最小可用（约 1 天）**：只做出向——`OnTakeFocus` → `take-focus`
  事件 → view emit `move-focus`，外加 `focus` vfunc 的"未运行则拒绝"。
  此时 Tab 序列为 `按钮1 → 按钮2 → view(文档聚焦, 无元素) → html按钮1 →
  html按钮2 → 按钮3`：边界已打通，只是从按钮2 到 html按钮1 需要按两次 Tab
  （第一次进入页面文档，第二次才聚焦第一个元素）。主诉求在此阶段已解决。
- **Phase 2 · 无缝入向（约 1 天）**：`focus-advance` 命令 + 入页合成 Tab +
  防回环 guard。实现 `按钮2 --Tab--> html按钮1` 一步直达。
- **Phase 3 · 打磨**：文档、示例、方向键 keynav 映射
  （`GTK_DIR_UP/LEFT≈backward、DOWN/RIGHT≈forward`，或仅抓焦点不推进）、
  单 focusable 退化场景优化（见下）。

## 边界情况

| 情况 | 行为 |
| --- | --- |
| 页面无可聚焦元素 | 入页合成 Tab 立刻招致 `take-focus`，guard 让 GTK 跳过 view，Tab 直达下一个 GTK 控件；点击仍可进入（游戏类键盘监听页面）。 |
| 页面 `preventDefault` 劫持 Tab | 合成键与真实键同样被页面吃掉——与浏览器行为一致，不视为 bug。 |
| 页面加载中进入 | 元素未就绪 → 立即跳出，等价于浏览器在空文档上按 Tab。可接受；不建议默认在 load-end 后补发推进（会抢用户的焦点操作）。 |
| 多 browser / 标签页 | `take-focus` 带 browser id，session 只在等于当前选中 browser 时生效；键盘本来就只送键盘焦点 surface，通常天然一致，id 比对是保险。 |
| 窗口里只有 view 一个可聚焦控件 | `take-focus` → move-focus 找不到别人 → guard 拒绝重入 → GTK 清空焦点；再按 Tab 重新进入页面。Phase 3 可选优化：识别"焦点落回自己"时改为合成入页 Tab，模拟浏览器式页内回绕。 |
| IME / text-input | 无需改动：焦点离开/回到 view 已由 `has-focus` 通知驱动 `keyboard_active` → `wl_keyboard` enter/leave → text-input enable/disable。 |
| AT-SPI（Orca） | 跨边界后叙述跳变等同于切换到原生控件，不阻塞。 |
| 用户狂按 Tab 与合成键交错 | guard 状态在回到主循环/焦点落定后清除，避免状态卡死；出向事件驱动不会热循环。 |

## 备选方案（仅当 Phase 0 失败）

- **B1 · 注入 JS 边缘侦测**：helper 已有脚本注入点，注入 capture 阶段
  `keydown` 监听，在 `Tab` 且 activeElement 为启发式首/末 tabbable 时发
  renderer→browser 进程消息（复用 `page-message`/`primary-selection` 同款
  通路）转为 `take-focus`。缺点：`querySelectorAll` 启发式对 shadow DOM、
  动态 DOM、radio group 不准。作为兜底可用，不作为首选。
- **B2 · `OnPreKeyEvent` 拦 Tab + `run_javascript` 查询 activeElement**：
  同样是启发式，且每键一次 JS 往返有竞态，不推荐。

首选 `OnTakeFocus` 的理由：它是 CEF 为该场景设计的唯一回调，语义精确
（由 Blink 的焦点遍历决定"到边缘了"），零启发式、零 JS 注入。

## 风险

- `OnTakeFocus` 的触发依赖 Chromium 内部 TakeFocus 通路在 Alloy+Wayland
  窗口下工作——Phase 0 先证实。
- `move-focus` 冒泡依赖 GtkWindow 的 class handler 做窗口级遍历，是 GTK
  公开信号的标准用法（GtkTextView 同款），风险低。
- 合成 `CefKeyEvent` 需与真实按键的 `type/modifiers/native` 字段一致，
  按 CEF `SendKeyEvent` 文档构造并实测。

## 落地记录（2026-10-25）

Phase 0–2 已全部落地并验证：

- **Phase 0 通过**：临时探针（加载完成后连发合成 Tab）实测 CEF
  154.0.34+chromium-154.0.8037.98、Alloy 窗口、嵌套 Wayland 下，Tab 到达
  页面边缘触发 `OnTakeFocus(next=true)`，反向触发 `next=false`；同时证实
  `CefBrowserHost::SendKeyEvent` 合成的 Tab 能被 Blink 当作真实遍历键处理
  （合成键从"无焦点"起步会聚焦第一个元素——正是入向所需的语义）。
  注：CEF 154 已移除 `SendFocusEvent`，焦点随 wl_keyboard enter/leave 走。
- **实现**（与方案的差异）：
  - helper：`OnTakeFocus` → 事件 `take-focus {browser, next}`；命令
    `focus-advance {forward}` → `FocusAdvanceTask` 用 `SendKeyEvent` 发
    RAWKEYDOWN+KEYUP 的 VK_TAB（按需带 SHIFT 修饰）。
  - view：覆写 `focus` vfunc；公开
    `chromium_view_advance_page_focus()`（入向推进，宿主也可直接调）与
    `chromium_view_release_chain_focus()`（出向让出，session 调）；
    `::focus-advance` 信号由 session 转成 `focus-advance` 命令。
  - "未运行"判据不是 `target==NULL`（那只有 demo 会设置），而是
    `get_paint_stack_for()` 非空——屏幕上有页面才参与焦点链；直接
    `grab_focus()` 不经 vfunc，demo 启动即抓焦点的行为不受影响。
  - 事件 `take-focus` 不按 browser id 过滤：只有握着键盘的页面能走到
    TakeFocus，而那正是 view 显示的页面；`browser` 字段留给宿主参考。
- **端到端验证**：`examples/focus-chain.c`（gtk 按钮×3 + 页面按钮×2，
  自驱动自断言，含空页面防回环场景）11 项断言全绿：
  `gtk1→gtk2→页a→页b→gtk3` 正向全通、反向全通、空页面被焦点链跳过。
- 控制通道协议文档（`docs/control-channel*.md`）已补两个消息。

尚未做（原 Phase 3）：方向键 keynav 的 UP/LEFT≈backward、DOWN/RIGHT≈forward
映射（当前方向键进入只抓焦点不推进）；单 focusable 退化场景的"页内回绕"
优化；焦点链相关 README 段落。
