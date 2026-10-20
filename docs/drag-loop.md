# 页内拖放环：谁在什么时候发哪一条

日期：2026-10-21
问题：P2「拖拽三段式」的第一段——一次在**同一个页面里**开始并结束的 HTML5 拖放，要经
`wl_data_device` 走一圈。任务 #46–#50。开工前最大的不确定是：Chromium/aura 会不会把窗内拖
拽当成浏览器内部的事，用 views 自己的 `DragDropManager` 处理掉，根本不理会我们发的
`wl_data_device.enter`。这一篇钉住它**确实**理会了，并且把只有实测才知道的六条写下来。

三段式的后两段——出界交给 GTK 的 drag、以及反向拖入——还没做，见 §6。

## 1. 形状：一端是页面，另一端也是页面

`wl_data_device_manager` v3 的 `wl_data_device` 是双向中继。剪贴板那半边早就通了（见
`docs/clipboard-bridge.md`），拖放这半边在 helper 里同样是**一个客户端的两端**：载荷从 Blink
出去走 `wl_data_source.send`，回到 Blink 走 `wl_data_offer.receive`。

| 谁 | 发什么 | 我们这里 |
| --- | --- | --- |
| 客户端 `start_drag(source, origin, icon, serial)` | 拖拽开始 | `data_device_start_drag`（`src/nested-data-device.c`） |
| 我们 → 目的地 `data_offer` + `offer`×N + `source_actions` | 把载荷介绍给它 | 复用剪贴板抽出来的 `data_offer_introduce()` |
| 我们 → 目的地 `enter(serial, surface, x, y, id)` | 每次换 surface 一条 | `drag_enter()` |
| 目的地 → `accept(mime)` | 它要哪一种 | `data_offer_accept` → `wl_data_source.target` |
| 目的地 → `set_actions(dnd_actions, preferred)` | 它愿意做什么 | `data_offer_set_actions` → `drag_negotiate()` |
| 我们 → `motion(time, x, y)` | 同一 surface 内的移动 | `drag_update_destination()` |
| 松手：我们 → 目的地 `drop`（**没有参数**），→ 源 `dnd_drop_performed` | 落地 | `drag_drop()` |
| 目的地 → `finish` | 我拿完了 | `data_offer_finish` → `wl_data_source_dnd_finished` |
| 任何一步不成立：我们 → 源 `cancelled` | 作废 | `drag_cancel()` |

`leave` 和 `drop` 都不带参数——写这两个 request 时按直觉传 surface/mime 会被 libwayland 的
签名挡下来，协议里它们真的就是空请求。

## 2. 实测的环

`test/dnd-page.html` 把左上角一个 `draggable` 盒子拖到铺满整页的 `#target` 上，盒子里
`setData('text/plain', 'dnd-payload')`。脚本 `drag 150 100 150 500`：按下、12 次移动
（25 ms 一次）、松手。页面把每条事件连同 `ev.target.id` 和 `clientX,clientY` 记进
`window.dndLog`。真机转录（`./test/dnd-gate.sh loop`）：

```
demo: script 3 ok value=""                       ← 拖之前页面什么都没记
demo: nested: a drag started over surface id 17 carrying 3 types
demo: nested: dragged onto surface id 17 offering a client's payload, id 4278190081
demo: nested: drag settled on action 0x1 of source 0x1 and destination 0x1
demo: nested: button 0x110 released: the drag is let go
demo: nested: dropped chromium/x-web-custom-data as action 0x1
demo: nested: a drag concluded: the destination has what it was given
demo: script 6 ok value="dragstart[text/plain]@source 150,100|
  dragenter[...]@target 150,200|dragover[...]@target 150,233| ... ×10 ...
  drop(dnd-payload)[...]@target 150,499|dragend[text/plain]@source 150,499"
```

### 2.1 页面真的被 `enter` 驱动

`dragenter`/`dragover`/`drop` 的 `clientX,clientY` 就是脚本喂给座位的坐标，逐条对得上。这条
是整个第一段成立与否的分界：**只要 `enter` 没被理会，页面一个 drop 事件都不会有**。所以
嵌套 compositor 这条路走得通，不需要在 helper 侧加 `CefDragHandler` 之类的旁路。

### 2.2 目的地 accept 的不是 `text/plain`

Chromium 作为目的地时 accept 的是它自己的 `chromium/x-web-custom-data`，`chosen_action` 从
`set_actions` 拿到 0x1（COPY）。而源 `start_drag` 时带上来 3 个类型，页面 `dataTransfer.types`
只报 `text/plain` + `chromium/x-drag-id`——内部类型 Blink 自己吃掉，不给 web 内容看。

含义有两个：`accepted_mime` 只能是「目的地挑中的那一个字符串」，不能拿它当「页面会读到什么」
的证据；门禁里 `expect 'dropped chromium/x-web-custom-data as action 0x1'` 钉的是 Chromium 当
前的行为，不是协议要求。

### 2.3 页内这一圈没走我们的 fd 中继

实测全程没有 `nested: passing <mime> to the client that owns it` 那行——即目的地从没发过
`wl_data_offer.receive`。页面 `getData('text/plain')` 拿到的 `dnd-payload` 是 Blink 自己那份
`DragData` 直接给的，因为两端在同一个 renderer 里。

所以第一段证明的是**编排**通（enter/accept/action/drop/finish 全对，事件坐标对，页面拿到载荷
且 `dragend` 收尾），fd 中继本身在这里没被触发。中继代码和复制共用同一
`DataSource` 传输路径，那半边由剪贴板测试覆盖；真正把拖放的中继用上的是第二、三段（对端是
GTK 或另一个客户端）。别把 §2.1 读成「载荷已经跨过进程了」。

### 2.4 拒绝的页面自己会拒绝

拖到不处理 drop 的元素上（`drag 150 100 150 90` 全程留在源盒子里）实测：

```
demo: nested: a drag started over surface id 17 carrying 3 types
demo: nested: dragged onto surface id 17 offering a client's payload, ...
demo: nested: button 0x110 released: the drag is let go
demo: script 5 ok value="dragstart[...]@source 150,100|dragend[text/plain]@source 150,90"
```

accept 照发（还是那个内部类型），但 `set_actions` 不来，`chosen_action` 停在 NONE。我们那条
「进来了却什么都没说，先当它愿意」的宽容规则于是**没有**把这次放行成一次成功：`drop` 发出去，
Blink 自己在 DOM 层面判定这个元素不接，页面只剩 `dragstart`/`dragend`，载荷一个字都没过去。
换句话说：目的地沉默时把 `drop` 发给它，风险由目的地自己承担，页面该拒的照样拒得掉。这条规则
留着是因为 shm-client 探针要的是「不报错」，见 §4。

### 2.5 拖动图标是有身份的一种 surface

Chromium 传进来的 icon 是它自己的一个 surface（实测 262x162，没有 `xdg_surface`，没有
title）。三件事必须同时成立它才画得出来：

- `is_drag_icon` 给它角色：映射条件里它和「有 xdg_surface」并列，否则一个 xdg 角色都没有的
  surface 永远进不了画面；
- 位置 = hotspot + `wl_surface.set_offset`。`surface_offset` 以前是空槽，现在真的存下来，
  commit 时才生效；实测指针停在 (150,500) 时图标落在 (150,500)（offset 0）。
- 它排在绘制栈**最后**，比页面自己开的菜单还高——协议说图标就该浮在所有东西上面。

命中测试要把它排除：协议明写拖放图标的 input region 被忽略。不排除的话，图标跟着指针走，就
永远挡在指针和真正该收到 `enter` 的 surface 之间。

`drag-hold` + `surfaces` 实测：拖之前 `1 surface(s) paint`，按住时 `2`，松手后回到 `1`——图标
随 `finish`/源销毁一起退场，不会变成跟着指针飞的鬼影。

### 2.6 座位在拖拽期间被冻结，之后必须还回来

拖拽自己持有指针：`seat_retarget()` 不换 pointer focus，`notify_pointer_motion()` 早退到
`drag_motion()`，任何按下都被吞掉，松手就是落地。实测 `drag-hold` → `release` →
`click 400 400` 之后页面 `window.clicks` 是 1，`click@target 400,400` 也进了日志——座位确实
交还给了窗口本身。

顺带一条实测陷阱：脚本拖拽和真实鼠标共用**同一个**座位。人在用这台机器时，手的抖动会变成页
面里的 `dragenter`/`dragleave` 对（实测混进过 `@target 469,183`）。所以门禁只钉脚本钉死的几
个坐标（按下点、落地那一格），中间的顺序允许出现 enter/leave 交替。

## 3. 回收：谁先死就按谁拆

| 事件 | 处理 |
| --- | --- |
| `wl_data_source` 被销毁 | 先清掉 `drag->source` 再 `cancelled`，绝不往正在死的 resource 上发事件 |
| 目的地的 offer resource 被销毁 | 分两种：还没 drop 的视同放弃，清 `dest_offer` 然后取消；已经 drop 的**不能**再发 `cancelled`（没什么可取消了），改成 `drag_destroy()` |
| 某一端的 `wl_data_device` 被销毁 | `drag_forget_device()` 先拆来源还是目的的那一半，再取消 |
| dispose | 只 `drag_destroy()`，一条事件都不发 |

`drag_forget_device` 的两个坑都踩过一遍：先解引用再判空；以及只认目的地、来源端销毁时提前
`return`，留下半条拖拽。现在两个布尔各自判。

§2.4 那种「发了 drop 却没人 finish」的拖拽以前会挂在 `NestedCompositor.drag` 上等客户端自己
收尾——它销毁 icon（`surface_is_painted()` 要求 `wl_surface` 还在）和 offer，绘制栈当场回到 1，
下一次 `start_drag` 也会先取消上一条。现在不挂了：destination 销毁 offer 就是它说完了，
`dropped` 已经置位，于是走 `drag_destroy()` 而不是 `cancelled`，留下
`a drag ended without the destination finishing` 一行 trace。顺序不能反——`drag_destroy()` 之前
必须先把 `drag->dest_offer` 清空，不然拆到一半又从这个字段回来找自己。这条由 `refused` 那一跑
实测覆盖：`loop`/`twice` 走的是正常 finish，只有它是 drop 之后没人 finish。

还剩一个真没管的状态：source 端把 icon、offer 全都留着不放，也不 finish。没为此加超时。

## 4. 决定：不给版本 3 的请求发协议错误

`test/shm-client.c` 的 `--dnd-requests` 对**剪贴板**对象发 `data_source.set_actions`、
`data_offer.set_actions`、`data_offer.finish`，然后断言两条连接都不报错。严格的实现可以对这
些发 `invalid_finish`/`invalid_action_mask`，但那会让 `meson test` 红，更糟的是真机上
Chromium 在普通复制时也可能发这几条——为了一个不存在的赢家去杀渲染进程不值。所以这几条在
非拖拽对象上只记日志。空 request 槽会让 compositor abort 这件事仍然成立，见
`docs/empty-request-slot.md`，`audit-impl-slots.py` 覆盖它们。

## 5. 怎么验

```sh
./test/dnd-gate.sh              # loop / refused / twice / held，35 项
./test/dnd-gate.sh held         # 只看拖动图标那一段
```

跑之前先 `ninja -C builddir`。要看 compositor 的 trace，得把 stderr 接成管道：GLib 默认的
log writer 在 stderr 是**普通文件**时会丢掉 DEBUG 级消息，
`G_MESSAGES_DEBUG=all ./builddir/src/cefnest-demo 2>log` 里一条 `nested:` 都不会出现，
`2>&1 | tee log` 才有。门禁里 `run_demo` 用 `tee` 就是为了这个。

协议错误那半边由 `meson test -C builddir` 的 `seat-selftest-dnd-requests` 覆盖。

## 6. 还没有

- **出界的 GTK drag 桥**（第二段）：把页面里的拖拽在越过窗口边界时移交成 GTK 的
  `GdkContentProvider`。需要先在拖拽还活着的时候把载荷预取过来——§2.3 说明页内这一圈不会
  替我们调 `receive`，出界时得由宿主自己发起读取。
- **拖入方向**（第三段）：`GdkDropTarget` → `wl_data_device.enter` + 一个新 `DataSource`。
- 拖动图标目前是客户端给的 surface，宿主没有替换成 GTK 的拖拽图像。
