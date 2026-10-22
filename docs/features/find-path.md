# 查找：一次请求几条回答，和它什么时候动页面

日期：2026-10-21
问题：P2 的「查找栏（CefFindHandler + UI，1 天）」，任务 #40–#45。`docs/guides/control-channel.md`
第 4 节以前把页内查找放在「没做」那一列。这一篇钉住 CEF 154 find 回调的形状——四条只有
实测才知道的语义，其中一条推翻了我照着「一个搜索两条更新」写下的推断。

## 1. 形状：命令没有回答，事件就是回答

| CEF 侧 | 通道上 |
| --- | --- |
| `CefBrowserHost::Find(text, forward, matchCase, findNext)`，**没有返回值** | `find` 命令，带 `pattern`/`forward`/`match_case`/`next`，**不发 ack** |
| `CefFindHandler::OnFindResult(browser, identifier, count, selectionRect, activeMatchOrdinal, finalUpdate)` | `find-result` 事件，四个字段（`selectionRect` 不上线，§2.4） |
| `CefBrowserHost::StopFinding(clearSelection)` | `find-stop`，实测一条回答都不发 |
| `CefClient::GetFindHandler()` | `helper/src/main.cpp:459` 的 `HelperFindHandler` |

`pattern` 每次都带（`next=true` 也是同一段文本），**helper 里没有任何查找状态**——和下载那条
「挂住的回调」正好相反，这里没有会过期的东西：宿主忘了自己在搜什么，重新发一条就是。

空 `pattern` 单独处理。CEF 头文件明写「空 searchText 等于停止搜索」，但那和 `find-stop` 是
同一件事的两种写法，`find` 直接拒（`status: find-needs-pattern`），
`chromium_session_find()` 也在 `g_return_if_fail` 里挡一道。停止只有一条路，宿主不必猜另一种。

## 2. 实测的四条

真机转录（页面 `test/find-page.html`：七个 `mango` 加一个 `Mango`）：

```
demo: find pattern=mango forward=1 match_case=0 next=0
demo: find result id=0 count=8 ordinal=0 final=0
demo: find result id=0 count=8 ordinal=0 final=1
demo: find pattern=mango forward=1 match_case=0 next=1
demo: find result id=1 count=8 ordinal=1 final=0
demo: find result id=1 count=8 ordinal=1 final=1
demo: find pattern=mango forward=1 match_case=0 next=1
demo: find result id=2 count=8 ordinal=2 final=0
demo: find result id=2 count=8 ordinal=2 final=1
demo: find pattern=nomatch-xyz forward=1 match_case=0 next=0
demo: find result id=3 count=0 ordinal=0 final=1
demo: find stop clear=1                        ← 之后一个 find result 都没有
```

### 2.1 `identifier` 每次请求 +1，而且从 0 开始

不是每次搜索一个号。头文件把它叫 "search identifier"，读起来像一次搜索一个；实测**步进也拿
新号**，起点是 **0**（fake helper 的起点是 1，是我随手写的）。所以真机门禁只写 `id=[0-9]+`，
fake 门禁才逐条钉 `id=1..5`：这个数字**只能用来把同一次请求的多条回答归到一起**，别的一律
不能假定，包括它从几开始。

### 2.2 有匹配：至少一条 counting + 一条 final；没匹配：只有 final

「至少」是这条的重点。第一版我按「一问两答」推断，实测里 `上一个` 紧跟一次新搜索时来了
**三条**（§2.3）。宿主唯一该信的是 `final`：非 final 的行只说明「还在数」，不该拿它当总数，
也不该拿它当第二次机会。反过来，`count=0` 且 `final=1` 就是「没匹配」的全部表达——CEF 154
没有单独的 no-match 标志。

### 2.3 新搜索不选中任何一条；ordinal 0 时按「上一个」会绕到最后一条

```
demo: find pattern=mango forward=1 match_case=1 next=0
demo: find result id=4 count=7 ordinal=0 final=0
demo: find result id=4 count=7 ordinal=0 final=1
demo: find pattern=mango forward=0 match_case=1 next=1
demo: find result id=5 count=7 ordinal=0 final=0     ← 同一次请求，另一条 counting
demo: find result id=5 count=7 ordinal=7 final=0
demo: find result id=5 count=7 ordinal=7 final=1
```

一次新搜索报 `ordinal=0`，所以 UI 在 final 之后只能说「已找到 7 处」，不能说「第 0 / 7 处」——
这条区分只能做在宿主侧（`src/main.c:638`，文案见 §3）。而 ordinal 0 时按「上一个」，Blink 往下绕到
最后一条（7），并为此多发那条 `ordinal=0` 的 counting。fake helper 复刻了这条，`fake-wrap`
门禁就是钉「宿主不能假定只有两条更新」。

还测到一次**偏移**：三次里有一次，新搜索后的第一步报 `ordinal=3` 而不是 1。真机门禁因此只断言
「三条 final、序号落在 1..8 之间」，不断言具体序列——那是 Blink 的内部顺序，不是通道契约。

### 2.4 搜索不动页面，**步进**把选中的那条滚进视野

`test/find-page.html` 里 `belowthefold` 只出现一次，在 3000 px 高的 spacer 下面。同一个脚本读
`document.scrollingElement.scrollTop`：

```
find belowthefold → count=1 ordinal=0 → scrollTop 0
find-next         →           ordinal=1 → scrollTop 2751.199951171875
```

这才是 `selectionRect` 不上线的完整依据：浏览器自己把**它选中的**那条滚进视野（宿主不必也不
该去滚动），而宿主既然画不进页面，一个窗口坐标对它没有用处。第一版我漏了括号里那半句就写
「Chromium 会自己滚进视野」，门禁立刻红在 `scrollTop is 0`——不是 bug，是我的推断只测了一半。
现在 `real-scroll` 两次都读，两边都断言（< 50 和 > 500）。

`match_case` 也在这张表里被证明真的到了 Blink：同一页面 `mango` 折大小写是 8，区分大小写是 7。
宿主侧做个 `lower()` 是不行的，数字得由浏览器给。

## 3. demo 面板做了什么

`src/main.c` 的 find 区只做两件事：把「搜什么、往哪个方向」变成命令，把 `::find-result` 变成
一行文案。

- **Ctrl+F** 打开并聚焦，**Esc** 关闭并撤高亮。按键接在窗口 `GtkEventControllerKey` 的
  **capture 阶段**（`gtk_event_controller_set_propagation_phase`），因为 `GtkEntry` 会先吃掉
  按键，而页内 Esc 又是 helper 读成「关窗口」的那一条（`docs/features/session-lifecycle.md`）。
- 「上一个」/「下一个」按钮、区分大小写勾选、输入框（回车 = 新搜索）、计数标签、关闭图标。
- 文案五态：`查找中…`（非 final，还没数出）、`已找到 N 处…`（非 final，数到 N 了）、
  `已找到 N 处`（final、ordinal 0 —— §2.3 的新搜索不选中任何一条）、
  `无匹配`（final、count 0）、`第 k / N 处`（final、有 active）。中间那一态是写完这篇之后
  才补的：第一版直接把 final+ordinal 0 渲染成「第 0 / 8 处」。
- 关闭发 `find-stop clear=1`，把 Chromium 的高亮一起撤掉——留着的话页面上会有个宿主已经
  看不见的选中框。
- 脚本步骤 `find <词>` / `find-case <词>` / `find-next` / `find-prev` / `find-stop` 走的是同一段
  代码（`src/main.c:1150` 起），无头跑的就是面板在跑，没有第二套实现。

## 4. 门禁

`./test/find-gate.sh`（`fake` / `real` / 两者）六次运行、34 条断言：

| 场景 | 钉住的东西 |
| --- | --- |
| `fake-counts` | 每请求一个新 identifier；`count=6`；步进 1→2→1；四条 counting；`count=0` 只答一次 |
| `fake-case` | `match_case` 作为字段过线，0/1 的差异来自对端 |
| `fake-stop` | `find-stop` 静默；停止之后到下一条请求之间没有事件 |
| `fake-wrap` | 一次请求可以有三条更新；ordinal 0 时「上一个」绕到最后一条 |
| `real-page` | 真页面 8 / 区分大小写 7 / 无匹配答一次；三条 step final 落在 1..8 |
| `real-scroll` | 搜索后 `scrollTop < 50`，步进后 `> 500`（实测 0 → 2751.2） |

`real-*` 那两次要等：载入 2.5 s，每条请求之后 0.7–1.8 s。扫页时长在真浏览器上不可预知，脚本
步骤用 `wait` 兜住节奏，而 `final` 事件本身才是「答案到了」的证据——门禁断言的是事件，不是
等待有没有过完。

一次**没归因**的现象记在这里，不藏：有一次 `./test/find-gate.sh real` 一条 `find result` 都没
收到（7 条断言全红），而逐字复现和之后的门禁跑全都正常。没有第二次复现，也就没有结论；下次
再出现时先看 helper 的收尾时序。

它开真窗口、跑真浏览器，所以和 `test/*.session`、`test/download-gate.sh` 同一档：手跑门禁，
不进 `meson test`。`meson test -C builddir` 14/14 与
`python3 test/audit-impl-slots.py builddir`（0 uncovered）不受影响。
