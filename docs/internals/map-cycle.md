# 每次上屏重新挂接的东西，必须有拆下来的路

日期：2026-10-22。来源：`docs/code-review-2026-10-22.md` **M11**。

> `src/chromium-view.c` — `chromium_view_watch_drops` 每次 map 重复添加
> `GtkDropTarget`，unmap 不移除 → 一次拖放触发 N 次合成 drag；
> `window-fullscreen-request` 连接（370-372）同样只增不减。

核实结论：**两条都成立**，而且都在 library 自己身上（不是宿主的用法问题）。

## 1. 为什么它是静默的

GLib 为每一次 `g_signal_connect()` 保留一个独立的 marshalling slot，GTK 为每一次
`gtk_widget_add_controller()` 保留一个独立的控制器。两者"随控件一起挂一次"都很便宜；
从 `map` 处理器里挂就变成一叠，因为窗口会被隐藏再显示——切标签页、最小化、切工作区、
关到托盘——**每一个周期多挂一份**。于是一次右键回五个菜单，一个拖入的文件把整条
drag-in 序列跑五遍：第一个处理器抢到事件，后面几个在已经不参与这轮信号的控件上照样
被调用。没有日志，没有告警，`g_signal_handler_find()` 之外没人知道槽位数在涨。

这条路径在多浏览器改造里被大量踩到：demo 的标签页切换就是把 view 从窗口里摘出来再放
回去，一次切换一个 map/unmap 周期。

## 2. 两种诚实形状

1. **随控件挂一次**：放在 `init`/`construct`，结构上就不可能重复；
2. **每个周期挂、同一个周期拆**：`map` 里连，`unmap`/`dispose` 里按存下来的
   handler id 断开。

判据只有一个：代码能不能把它拆回去。挂的目标是不是"正在被 map 的那个控件"不重要——
挂在控件自己身上的槽位同样是每个周期多一个。

## 3. 改动

| 挂接点 | 改前 | 改后 |
| --- | --- | --- |
| `::window-fullscreen-request`（compositor → view） | `on_map` 里 `g_signal_connect_object`，返回值丢弃 | `chromium_view_init` 里连一次，id 存 `fullscreen_request_handler`；`dispose` 断开 |
| 外部拖放的 `GtkDropTarget` | `on_map` 里 `chromium_view_watch_drops (self)` | `chromium_view_init` 里加一次 |
| 共享 compositor 的第二个 view | 每个 view 各自回答同一个请求 | `chromium_view_use_shared_compositor()` 断开且**不再重连**：画别人显示器的 surface 不替那个显示器回答全屏请求 |
| root 窗口 `notify::fullscreened` | 每个周期挂、`unmap` 拆 | 不动，这是第 2 种诚实形状（运行门禁专门量它） |

## 4. 门禁一：`test/map-cycle.c`（运行，量槽位数）

把 view 从窗口里摘出来再放回去，跑 5 个周期，然后用 GLib 自己的账本读数：
`g_signal_handlers_block_matched()` 会回答"这个对象的这个信号有几个处理器"，
当场 block 再 unblock，中间不回事件循环，所以谁也发不出来。

它同时量**允许的形状**：`notify::fullscreened` 在窗口上屏期间应该是 1 个、离开屏幕
应该是 0 个——两个就说明拆的那一半没了，零个就说明挂的那一半没跑。

共享 view 那段还钉住一条：`chromium_view_new_shared()` 起来的 view 反复上下屏，
compositor 上的 `window-fullscreen-request` 槽位数仍是 1（只有 owner 回答），
而 `frame` 是 2（owner + 共享者），共享者自己那个窗口上一个 `notify::fullscreened`
都没有。

**反空跑**：先证明周期真的发生了——映射次数必须是 6（首次上屏 + 5 个周期）、解除
映射 5 次，对不上就直接 `return 1` 并写"this run graded nothing"，否则下面每个数字
都只是"一个只上过一次屏的窗口"的数字。

绿：

```
ok: window-fullscreen-request slots before any showing is 1
ok: frame slots before any showing is 1
ok: the view was hidden and shown 5 times
ok: root notify::fullscreened slots at the top of a showing is 1
ok: root notify::fullscreened slots at the bottom of a hiding is 0
ok: window-fullscreen-request slots after the showings is 1
ok: frame slots after the showings is 1
ok: text-input-state slots after the showings is 1
ok: window-fullscreen-request slots with a shared view over them is 1
ok: frame slots with the owner and a shared view is 2
ok: notify::fullscreened slots on a shared view's own window is 0
map-cycle: nothing a showing armed is left over
```

**变异 A**（把丢弃返回值的连接放回 `on_map`，就是 M11 原本的样子）→ rc=1：

```
map-cycle: window-fullscreen-request slots after the showings is 7, expected 1
map-cycle: window-fullscreen-request slots with a shared view over them is 7, expected 1
map-cycle: 2 failure(s)
```

7 = init 的 1 + owner 窗口的 5 个周期 + 共享窗口首次上屏的 1。

**变异 B**（把 `on_unmap` 里拆 `notify::fullscreened` 的那段删掉）→ rc=1：

```
map-cycle: root notify::fullscreened slots at the bottom of a hiding is 1, expected 0
map-cycle: 1 failure(s)
```

B 之后 `watch_fullscreen()` 自己那句"先拆旧的"会在下一次上屏兜住，所以"上屏时是 1
个"照样过——**只有拆的那一半没了，才在"离开屏幕"那一侧露出来**。

## 5. 门禁二：`test/audit-map-arms.py`（静态，`protocol` 套件）

运行门禁量不到的东西必须有人在源码上拦：GTK4 没有公开的控制器列表，
`gtk_widget_add_controller()` 叠多少个，进程里数不出来。这条审计读 `src/*.c`，
从每个 `*_on_map` 的函数体出发（再跟进一层同文件被调函数，因为控制器其实是
`chromium_view_watch_drops()` 里挂进去的），报告任何一处：

- 返回值被丢弃，或
- 存进了一个本文件从不解绑的 `self->field`。

解绑的证据接受 `g_signal_handler_disconnect (obj, self->field)`、
`g_clear_handle_id (&self->field, ...)` 和 `gtk_widget_remove_controller (obj, self->field)`。
赋值和连接常常不在同一行，所以判"有没有被存下来"是从连接点往回看上一句，不是指望
它写在一行里。

绿：

```
ok: every arm made from a map handler can be taken down again
```

**变异 A'**（把 `chromium_view_watch_drops (self)` 和那句丢弃返回值的连接放回
`on_map`）→ rc=1，三处一起报出来：

```
map-handler arms that cannot be taken down:
  src/chromium-view.c:341: g_signal_connect from a map handler, and the id is thrown away, so it cannot be taken down
  src/chromium-view.c:342: gtk_widget_add_controller from a map handler, and the id is thrown away, so it cannot be taken down
  src/chromium-view.c:371: g_signal_connect_object from a map handler, and the id is thrown away, so it cannot be taken down

Arm them with the widget (init), or store the id and disconnect it in the matching unmap/dispose.
```

341/342 正是 helper 里真正挂控制器的那两行，371 是 `on_map` 里那句被丢弃的连接。

**变异 B'**（把 `on_map` 里的连接存进一个没人解绑的字段）→ rc=1：

```
  src/chromium-view.c:371: g_signal_connect from a map handler, and stored in self->im_focused, which nothing disconnects
```

## 6. 两条门禁各拦什么

| | `map-cycle`（运行） | `map-arms-audit`（静态） |
| --- | --- | --- |
| 挂而不拆（变异 B） | **红**（离开屏幕时还剩 1 个） | 绿——字段确实有人解绑，静态看不出解绑少了一半 |
| 控制器叠加（变异 A 的 342 行） | 看不见——GTK4 不公开控制器列表 | **红** |
| 返回值丢弃 | 若是信号则**红**；控制器测不到 | **红** |

所以两条都要留着：一条量 GLib 的账本，一条读源码的形状，各自都能报出另一件报不出来的
事。

## 7. 还没做完的部分

M12 是同一段代码的另一半：`::window-fullscreen-request` 没有"哪一个 surface 提的
请求"这个参数。共享 compositor 时，view 现在只有 owner 一个回答者，因此不会五扇窗一起
全屏；但**该全屏的究竟是哪扇窗**仍然只能由 owner 猜（它自己的 root 窗口）。把
`NestedSurface *` 带进信号、由 view 判断"这是不是我 this view 画的那扇窗"，需要动
`src/nested.c` / `nested-xdg.c` 的信号签名，本轮没有做。
