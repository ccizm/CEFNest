# 会话恢复配方

[English](session-restore.md) | 简体中文

浏览器壳子的会话恢复——窗口重开时回来的那些标签页——**不需要组件做任何事**：
这是一份宿主用既有 API 就能执行的配方。组件本身刻意不保存任何会话状态
（helper 的 profile 是 Chromium 侧仅有的状态，归 data-directory 契约管）；
下面是宿主侧的形状。

## 存什么

每个窗口在关闭时（或按周期，兼作崩溃保护）：

1. 标签页列表，按顺序，标出激活的那个。
2. 每个标签页：当前 URL（来自 `::address` /
   `chromium_session_browsers_list()`），以及**后退栈**——一次
   `chromium_session_history(browser, id)` 调用，其 `::history-result`
   携带 `current_index` 与 `entries[]`（index、URL、标题、哪条是当前页）。

两者都按你自己的 JSON 存。浏览器 id 是 CEF 的，每次 `start()` 都会变，
除此之外这些数据在你的进程之外没有任何意义。

## 恢复什么

下一次 `start()` 时，按保存的顺序逐个标签页：

1. `chromium_session_new_browser(url, windowed)`——想避免二次加载就传空
   URL（新 browser 本来就落在 `about:blank`）。
2. 等 `::browser-created` 给出新的 browser id，然后对它跑一次
   `chromium_session_history()` 看它现在的位置。
3. **倒着**重建后退栈：从保存的 current_index 前一条开始一路降到 0，
   逐条 `chromium_session_history_go(browser, index)`。`history_go` 走组件
   自己的管道（CEF 公开 API 只能读导航历史、不能跳），全程不涉及
   `cdp_enable`；每次跳转都以常规 `::load-started` / `::load-end` 呈现。
4. 最后 `chromium_session_select_browser()` 选回保存时激活的标签页。

两个事实让这套流程精确而非近似：

- `history()` 报告的是 **Chromium 仍持有的**条目，远处的条目会被裁剪。
  存它报的；恢复时 `history_go` 还收多少算多少——越界的 index 在 status
  通道上以 `history-go-failed` 应答，栈就到那里为止。
- `history_go` 没有回执。用被恢复浏览器自己的 `::load-end` / `::address`
  事件驱动恢复流程，一次一跳，它就和所有导航一样可观察。

值得用后退栈而不只是 URL 列表的原因就在这里：恢复出的标签页保留自己的
历史菜单，Back/Forward 立刻可用。

## 不要期待什么

- 滚动位置、表单数据、每标签页的缩放都挂在旧 browser 对象的 Chromium 侧
  状态上；恢复出的标签页从页顶开始。
- 会话 cookie 不活过 helper 重启（Chromium 自己的规则，有无 data 目录
  皆然）——依赖它的恢复标签页会落在登录页。真浏览器在站点 cookie 不持久
  时也是这个行为。
- `ephy-session` 式的崩溃恢复只差保存时机：同一配方，把"关闭时存"换成
  "定时存"。
