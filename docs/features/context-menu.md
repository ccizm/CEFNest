# 上下文菜单：谁画它，和谁欠它一个回答

日期：2026-10-21
问题：P2 的「上下文菜单原生映射（CefContextMenuHandler → GtkPopoverMenu + 协议扩展，2~3 天）」，
任务 #51–#56。做完之后有一件事和开始时的假设不一样：**这不是补一个缺失的功能**。CEF 自己已经把
菜单画出来了，而且能点。这一篇钉住三件只有实测才知道的事——右键到底走到了谁那里、一条没被回答的
请求值多少钱、以及 `Continue(id)` 到底做了什么。

## 1. 动手之前：今天右键做了什么

先把「右键到哪儿去了」测清楚，因为它决定这一条到底要不要做。

- **右键的内核按钮码是 `0x111`（273）**。第一次探针写的 275/0x113 是 `BTN_SIDE`，页面读成
  `ev.button === 3`（浏览器后退），既没有 `contextmenu` 也没有菜单。宿主侧
  `src/chromium-view.c` 的 `NESTED_BTN_RIGHT 0x111` 一直是对的——是探针错了，不是产品错了。
- 一次真的右键**完整走到了页面**：
  `mousedown2@link|contextmenu@link 140,40|mouseup2@link`，坐标和脚本喂进嵌套 compositor 的逻辑
  像素一致。
- 而 CEF 默认（`RunContextMenu` 返回 false）会在 widget 里画**它自己的** Views 菜单：一个
  xdg_popup，content 136x310、surface 184x358（24 px 阴影内缩），放不下时自己挪位置
  （151,272 → 127,260）。它是**可以点的**：脚本点中「全选」那一行，页面就报 `edit:0-18`；
  点阴影只是关掉。

所以「原生映射」换的不是能力，是**外观和控制权**：宿主拿到模型，自己决定文案、顺序、主题，以及加
自己的行。这个区别值得写在开工之前——如果 CEF 的菜单根本不显示，这一条就是修 bug；它显示，那这一条
就是做集成。

## 2. 一条决定全部设计的实测：未回答的请求会吃掉后面每一次右键

helper 一接管（`RunContextMenu` 返回 true）而宿主不回话，五次右键只来**一条** `context-menu` 事件：

```
helper: context menu with 5 items at 140,40      ← 只有这一次
demo: ... title=MENU-2 / MENU-4 / MENU-7 / MENU-10 / MENU-13
                                                 ← 页面五次都收到了 contextmenu
demo: script 13 ok value="14 MENU-13"
demo: menu dismiss                               ← 回答掉手里这条
helper: context menu with 5 items at 140,40      ← 第六次右键又有菜单了
```

`OnContextMenuDismissed` 的头文件说法是「不管结果如何都会调」，但**请求槽只有一个**：CEF 不再为第二
次右键建模型。所以 `src/chromium-session.c` 的默认行为是「**一定回答**」：

- 宿主不接管 → session 画自己的默认菜单（画出来就是一条等着被点的菜单）；
- 没有 view、或模型里没有可点的行 → 直接 `context-menu-dismiss`，不留挂起的请求；
- 两条公开 API 都在本地挡掉重复回答（`session: no context menu to answer`），不发第二条命令。

**顺带测到一条推翻直觉的**：页面导航**不会**关掉一条被宿主接管的请求。没有
`context-menu-dismissed`，菜单继续挂在屏幕上新页面的上面，后面的右键也还是没有事件——CEF 认为「宿主
正在显示它」，那和页面换没换无关。所以 `loading` 事件到达而 `menu_pending` 还立着时，session 自己
dismiss（`session: context menu dismissed by a navigation`），`real-nav` 门禁钉的就是这条：导航后
第二次右键又有菜单，而且 `返回` 那行从 disabled 变成 enabled。

## 3. 形状：模型过线，params 留在 helper

| CEF 侧 | 通道上 |
| --- | --- |
| `RunContextMenu(browser, frame, params, model, callback)` → return true | `context-menu` 事件：`x`/`y`/`items[]`，**不发 ack** |
| `CefRunContextMenuCallback::Continue(command_id, event_flags)` | `context-menu-select`，带 `id`，`flags` 可选 |
| `CefRunContextMenuCallback::Cancel()` | `context-menu-dismiss` |
| `OnContextMenuDismissed` | `context-menu-dismissed`，**只在 helper 还握着请求时**发 |
| `CefClient::GetContextMenuHandler()` | `helper/src/main.cpp` 的 `HelperContextMenuHandler` |

一条 item 五个字段：`id` / `label` / `type` / `enabled` / `checked`。分隔行的 id 是 **-1**、label 是
**空串**、enabled 恒为 true；`type` 的拼写是 command / check / radio / separator / submenu / none。
`IsVisibleAt(i)` 为 false 的行**不上线**——宿主看到的模型就是该显示的菜单。

`CefContextMenuParams` 留在 helper 进程里，但不再是“只用来回答模型为什么长成这样”：helper 现在
自己读 `GetTypeFlags()` / `GetMediaType()` / `GetLinkUrl()` / `GetSourceUrl()`，在默认模型前面插入
链接/图片/媒体的浏览器动作（见 §4 和 §9）。宿主仍然只拿模型和 id，不知道 params；要由宿主加自己的
行，才需要把 params 上线，那是下一条协议扩展。

## 4. 实测的模型：CEF 的默认模型很薄

真机转录（`test/menu-page.html`，五个上下文各点一次，每次都回答掉）：

| 点在哪里 | 模型 |
| --- | --- |
| 链接 (140,40) | **2 行**：在新窗口打开链接 / 复制链接地址（不再带返回、前进、打印、查看源代码） |
| 图片 (120,130) | **3 行**：在新窗口打开图片 / 复制图片地址 / 图片另存为…（同上，不带通用导航行） |
| 空白页面 (400,300) | 5 行：返回/前进(off) \| sep \| 打印 / 查看网页源代码 |
| 文本框内 (100,250) | **9 行**：撤消/重做(off) \| sep \| 剪切/复制(off) 粘贴/粘贴为纯文本(on) \| sep \| 全选(on) |
| 段落有选中 (400,45) | **1 行**：`复制(&C)`#113(on) |

「复制链接地址」「图片另存为」**不在 libcef 的 Alloy 默认模型里**——CEF 的 `cef_menu_id_t` 根本没有
这些 id。它们现在由 `HelperContextMenuHandler` 按 params 插入（id 26501–26507）：宿主只负责画行，
选中后 `OnContextMenuCommand` 在 helper 里执行。图片保存复用 `StartDownload`，到宿主侧用
`GtkFileDialog` 选择路径；`CEFNEST_DOWNLOAD_NO_SAVE_AS` 可以退回普通下载目录策略。

两条测的时候绊到我的：

- `window.selectEdit()` 选了 0–8，之后在 (100,250) 右键，菜单里 `复制` 是 **disabled**。不是 bug：
  textarea 从 top:220 起，(100,250) 落在**第二行**，Blink 把光标移过去了，选中随之消失。要点在选择
  那一行才有 `复制(on)`——段落那次 (400,45) 就是。顺带：`<p>` 是 fixed 定位，`top:20px` 定位的是
  **margin box**，默认 1 em 上下边距把文字推到 y≈36，所以 (400,30) 命中的是 body 而不是段落。
- `params` 的 y 有时比页面的 `clientY` 大 1（120,130 → 120,131；100,250 → 100,251）。±1 px，没找到
  规律，放置不受影响，但门禁里凡是用坐标断言的地方都写成 `400,4[0-9]` 这种范围。

## 5. `Continue(id)` 跑的是 Chromium 自己的动作

`menu-pick 117` 之后，页面上 `selectionStart + '-' + selectionEnd` 回读 **`0-18`**——全选真的在页面
里执行了。头文件说 `OnContextMenuCommand` 返回 false 走「默认实现」，`Continue()` 就是那条默认实现
的入口：**宿主不需要自己实现复制、全选、查看源代码**，只要把 id 送回去。这条是 `real-models` 门禁的
最后一步，也是整件事唯一真正证明「映射通了」的地方——菜单画出来了不算，命令得在对端跑起来。

`flags` 是 `cef_event_flags_t`（中键、ctrl+click 之类），协议留了位置，API 上没开：
`chromium_session_context_menu_select()` 只发 id，`flags` 走 0。等真需要区分修饰键时再加，不要先建
一个没人用的参数。

## 6. 默认菜单这一侧

- **普通 `GtkPopover` + `GtkBox`，不再用 `GtkPopoverMenu`**。后者把内容包进 `GtkScrolledWindow`，
  还会给 section 留 padding；单行“复制”菜单因此底部空出一截，菜单行也点不中。现在每行是一个
  `GtkButton`（flat），分隔行是 `GtkSeparator`，按钮的 `clicked` 直接调
  `chromium_session_context_menu_select()`。行的数量决定菜单高度，没有隐藏的滚动容器。
- **位置用 `gtk_widget_set_halign (popover, GTK_ALIGN_START)`**：`GTK_POS_BOTTOM` 按 popover 的
  halign 选 anchor，默认 `CENTER` 会让菜单横跨指针；`START` 才是“菜单左边缘在指针上，向右展开”。
- **`(&B)` → `(_B)`**，label 里真的下划线写两遍。不转义的话 `(&B)` 会原样打印在词后面（CEF 的快捷
  键写法是 GTK 的转义写法的子集，反过来不成立）。
- check / radio 当普通 command 行画；submenu 直接跳过——这个扁平 popover 不展开子菜单，空的 submenu
  行只会留下一条空白。`MENU_ID_ADD_TO_DICTIONARY`（添加到字典）也在 helper 侧丢掉，因为内嵌组件
  没有字典/语言设置；`MENU_ID_NO_SPELLING_SUGGESTIONS` 保留，但 label 里的字面 `&` 会被清掉，不再
  以快捷键残渣的形式显示。这份菜单**没有状态可标记**：每条命令之后模型会整个重建，`checked` 因此
  只是信息，不是显示。fake 的 `mixed` 模型专门用来钉 submenu 的跳过不会崩。
- **popover 挂在 `ChromiumView` 上**（`gtk_widget_set_parent`）。GTK 的提示写着「父控件要支持 popover
  子控件，否则要在 `size_allocate` 里调 `gtk_popover_present()`」，而 `ChromiumView` 是个只有自定义
  snapshot 的普通 GtkWidget。**2026-10-21 的真实点击把这条假设打破了**：菜单会出现滚动条、显示不全，
  行也点不中——popover 的 surface 高度和输入区域停在了旧值。现在 `ChromiumView::size_allocate()` 会
  遍历 `GtkPopover` 子控件并调用 `gtk_popover_present()`，`session_menu_show()` 在 `popup` 前也补一次；
  修复后 9 行编辑菜单完整显示，不再出现滚动条。此前的尺寸记录（5 行 173x138、9 行 131x228、
  1 行 101x64）是**逻辑像素**（`gtk_widget_get_width()` 的单位，scale 1.25 下 surface 会更大），
  不是设备像素。
- **真实点击的另一个坑：`::closed` 可能先于行的动作到达**。`menu_popover_closed()` 原来会立刻把
  `menu_pending` 清掉并发送 `context-menu-dismiss`；如果 GTK 先 popdown、后发按钮回调，同一次点击的
  命令就丢了。现在 `::closed` 只安排一个 idle，由 `menu_release_later()` 在事件排空后决定：还 pending
  才 dismiss；同一击已经 `select` 就什么都不发。
- **菜单是非模态的，页面焦点不丢。** `gtk_popover_set_autohide(FALSE)` 避免 GtkPopover 的 modal
  keyboard grab；打开期间 `ChromiumView` 仍 `has-focus=1`，页面继续拥有键盘。关闭菜单不再靠
  popover 的 grab，而由 session 装的三个守卫负责：窗口上的 capture button 事件（点在菜单 surface
  上的排除）、capture Escape、以及窗口 `notify::active` 变 false。点击菜单按钮后仍保留原来的
  50 ms `grab_focus(ChromiumView)` 兜底；用户点了地址栏等控件则不抢。

  非模态菜单的键盘导航由 session 自己的窗口 capture key controller 提供，**焦点始终留在页面**：
  `Up/Down` 或 `Tab/Shift+Tab` 移动，`Home/End` 到两端，`Enter/Space` 激活，`Esc` 关闭；
  控制/Alt/Super 组合键不下拦，仍交给页面。选中行用 `cefnest-menu-selected` 高亮。鼠标点选仍然有效。
- Wayland 上 popover 是**它自己的 surface**，所以 `shot` 抓 view 抓不到它。这就是 session 要打那条
  `context menu mapped=1 size=...` debug 的原因：进程内唯一能证明「菜单到了屏幕」的地方，也是门禁
  断言它的地方。**但这条断言问的是显示器，不是菜单**：`./test/ci-session.sh` 那块没有输出设备的
  nested mutter 里，弹层照样建好、照样请求布局，surface 却永远不会被映射（实测恒为
  `mapped=0 size=0x0`），于是 `menu-gate.sh` 读 `CEFNEST_HEADLESS` 把它报成 skip，其余 45 条不变。
- **拆除只能等一个 idle**。popover 的 `::closed` 发完之后 GTK 还要继续用它（`_gtk_popover_close()`
  在 emit 之后仍要 snapshot 这个 child），而行的 `activate` 也在同一个弹层里——在两处之一里
  `gtk_widget_unparent` 都是把正在 emit 的对象拆掉。`menu_release_soon()` 因此挂
  `g_timeout_add_once (0, ...)`，并且 `menu_release()` 先断开 `::closed`：这一侧主动放下的菜单，回答
  已经定了，不该由关闭回调再补一条。
- `::closed` 是三种关闭的汇合点（Esc、点外面、窗口没了），也是宿主没接管时**唯一**会发 dismiss 的地方。

## 7. 宿主 API，和一个 GObject 信号的老坑

`::context-menu (x, y, items)`：返回 TRUE 表示「我画，我负责回答」；返回 FALSE 或不接，session 画默认
菜单。`items` 是 `GList<ChromiumMenuItem*>`，**只在信号发射期间归 session**，宿主想留要自己拷。

踩过的坑，记下来免得再踩：**gboolean 信号的返回值就是 handler 的返回值**，`g_signal_emit()` 最后那个
`&handled` 是收集结果的位置，**不是传给 handler 的参数**。第一版 demo handler 写成
`(session, x, y, items, gboolean *handled, data)`，于是 `handled` 收到的是 user_data（NULL），
`*handled = FALSE` 当场段错误（gdb 里 `handled=0x0, data=0x3e` 一眼见底）。没有 accumulator 时**最后
一个 handler 的返回值赢**，这条也写进了 `src/chromium-session.h`。

两条公开 API：`chromium_session_context_menu_select()` / `chromium_session_context_menu_dismiss()`。
真 helper 对「没有挂起请求的 select」回 `status: no-context-menu`，对「没有挂起请求的 dismiss」静默
（`Cancel()` 在空槽上就是静默），fake 照抄这两条。demo 的 `::context-menu` 只打印模型并返回 FALSE，
所以无头跑的就是库的默认菜单，没有第二套实现；步骤是 `menu-pick <id>` / `menu-dismiss`。

## 8. 门禁

`./test/menu-gate.sh`（`fake` / `real` / 两者）八次运行、47 条断言：

| 场景 | 钉住的东西 |
| --- | --- |
| `fake-link` | 五行逐字段一致；画出来了、`mapped=1`；`id=132` 过线；第二次 pick 只有本地挡，通道上一条命令 |
| `fake-edit` | 九行、两条分隔 → 三段；disabled 行照 disabled；dismiss 过线 |
| `fake-mixed` | check / radio / submenu / 带 `&` 和下划线的 label 都不崩；自定义 id 26501 过线 |
| `fake-empty` | 空模型和「只有分隔」模型都**不画**，但都答 dismiss；第三次 pick 没有可答的 |
| `fake-auto` | helper 自己关掉请求 → 菜单下来，且**不发**第二条 dismiss；下一条请求照常 |
| `fake-double` | 两条请求之间没回答 → 旧的静默放下，只有第二条被回答，全程没有 dismiss |
| `real-models` | 链接 2 行 / 图片 3 行 / 文本框 9 行 / 选中 1 行；`menu-pick 117` 后页面回读 `0-18` |
| `real-nav` | 导航关掉挂起的菜单；之后右键又有菜单，且 `返回` 变 enabled |

`real-*` 那两次要等：载入 3 s，每次右键之后 1.2 s（菜单要画、要 mapped、要被 snapshot 一轮）。

它开真窗口、跑真浏览器，所以和 `test/*.session`、`test/download-gate.sh` 同一档：手跑门禁，不进
`meson test`。`meson test -C builddir` 14/14 与 `python3 test/audit-impl-slots.py builddir`
（0 uncovered）不受影响。

## 9. 还没做的

- **params 不上线**，宿主仍然不能自己加需要 `link_url` / `src_url` 的行；链接/图片/媒体的内置动作
  由 helper 加了，宿主自定义行仍是下一步的协议扩展。
- 菜单被 Esc 或外部点击关掉，走的是 GTK 自己的 grab，**没有无头办法测**：注入的右键直接进嵌套
  compositor，绕开外层 grab。`fake-auto` 测的是同一段收尾代码的另一条入口。
- `flags`（中键、修饰键）在 API 上没开；check / radio 的勾选标记没画；submenu 不展开。
- 一条挂起的请求在 helper **退出**时会怎么样，没测：`context-menu-select` 发不出去，宿主那边
  `menu_pending` 立着，但 session 已经关了。等 `docs/features/session-lifecycle.md` 那侧有结论时一起看。

## 10. 拼写建议默认关闭；打开时菜单关掉后把光标收回来（2026-10-21）

**默认关闭**：helper 给 Chromium 加 `--disable-spell-checking`，并在每次主 frame
`load-end` 后把 `input/textarea/[contenteditable]` 的 `spellcheck` 设为 `false`（包括之后
新增的节点）。这样不会有拼写建议菜单，也不会先把 misspelled word 选中，粘贴问题从源头消失。
需要保留浏览器拼写体验时设 `CHROMIUM_SPELLCHECK=1`。

如果显式打开，仍保留下面的校验/恢复逻辑：

Chromium 在把拼写建议菜单交给宿主之前，会先把 misspelled word 选成当前选区。菜单关闭后
这个选区还在，下一次 Ctrl+V 就替换整个词，而不是插在光标处。

helper 在 `RunContextMenu` 里记录：

```text
IsEditable() && GetMisspelledWord() 非空
```

并在菜单回答后执行一段不经过 `eval` 的脚本，把 input/textarea 的 `selectionEnd` 折成
caret；contenteditable 则把 document selection 折到末尾。 **`MENU_ID_PASTE` /
`MENU_ID_PASTE_MATCH_STYLE` 要在 `Continue` 之前折**，这样菜单里的“粘贴”不会把选中的词
替换掉；**`MENU_ID_SELECT_ALL` 不折**，因为选区就是它的结果；其他命令（复制、建议、自定义
行）在 `Continue` 之后折，建议替换仍然按 CEF 自己的逻辑完成。helper 自己关菜单
（`OnContextMenuDismissed`）也会折。

影响范围是刻意收窄的：

- 只有 **可编辑且 `GetMisspelledWord()` 非空** 的菜单；
- 链接、图片、媒体、普通选区、没有拼写错误的编辑菜单都不折 caret；
- 菜单外的键盘 Ctrl+V 和菜单里的 Paste 都会在光标处插入，不会再替换整个词；
- 折到 `selectionEnd` 是“词后面”而不是原始点击位置；CEF 没有把原 caret 放到 params 里，
  这是当前能做的最小修复。若以后要精确到点击字符，需要把 CEF 的 selection/caret 信息
  一起上线，或做更细的页面事件记录。

本地 CEF 的 minimal profile 没有拼写字典，复现不出 suggestion 菜单；这条按 CEF 回调路径
实现，需在带字典的真实 profile 上手工确认。
