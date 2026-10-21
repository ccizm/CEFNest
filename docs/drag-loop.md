# 拖放环，和它越过窗口边界时的两跳：谁在什么时候发哪一条

日期：2026-10-21
问题：P2「拖拽三段式」的第一段——一次在**同一个页面里**开始并结束的 HTML5 拖放，要经
`wl_data_device` 走一圈。任务 #46–#50。开工前最大的不确定是：Chromium/aura 会不会把窗内拖
拽当成浏览器内部的事，用 views 自己的 `DragDropManager` 处理掉，根本不理会我们发的
`wl_data_device.enter`。这一篇钉住它**确实**理会了，并且把只有实测才知道的六条写下来。

三段式的第二段——拖拽越过窗口边界时交给宿主——在 §6。第三段方向相反，宿主把拖拽带进来、页面
当目的地，在 §7。

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
./test/dnd-gate.sh              # loop / refused / twice / held / out / in
./test/dnd-gate.sh held         # 只看拖动图标那一段
./test/dnd-gate.sh out          # 只看越界移交那一段
./test/dnd-gate.sh in           # 只看宿主拖入那一段
```

跑之前先 `ninja -C builddir`。要看 compositor 的 trace，得把两路输出合并成管道：进程一旦不在
tty 上，GLib 默认的 log writer 就把 DEBUG 级消息发到 **stdout** 而不是 stderr（实测同一次跑动
stdout 侧 302 条 DEBUG、stderr 侧 0 条），所以 `G_MESSAGES_DEBUG=all ./builddir/src/cefnest-demo 2>log`
里一条 `nested:` 都不会出现，`2>&1 | tee log` 才有。门禁里 `run_demo` 用 `tee` 就是为了这个。

协议错误那半边由 `meson test -C builddir` 的 `seat-selftest-dnd-requests` 覆盖；§6 的移交则由
`seat-selftest-drag-out` 用一个假客户端重放一遍，不需要浏览器，也不需要窗口。§7 的拖入同理，是
`seat-selftest-drag-in`。

## 6. 第二段：越界的移交（预取 + `::drag-out`）

### 6.1 为什么读的人必须是 compositor

§2.3 说过页内那一圈没人调 `receive`。越界时目的地更不是我们的客户端——对面是宿主，是 GTK——
所以「目的地 accept，再 receive」这两步只能由 compositor 自己扮演：先 `wl_data_source.target(mime)`
说清楚要哪一个类型，再 `send(mime, fd)`，把管道读空，然后 `cancelled` 把页面的拖拽收掉，最后
`::drag-out (mime, bytes, types)` 交给宿主。公开的三样是
`nested_compositor_drag_take_out()`、`nested_compositor_drag_get_types()` 和那个信号；返回值只说明
「有拖拽可拿」，东西要等信号到了才算到手，`bytes` 和 `types` 也只在 handler 里有效。空载荷不发
信号：源端不回答，就等于没有移交。上限仍是复制那一份 4 MiB。

两处的顺序都是被重入教出来的：传输对象在调用 finish **之前**销毁（finish 会取消拖拽，取消又会来
销毁这条传输），类型列表在取消**之前**抄下来（源一旦被告知 `cancelled`，客户端随时可能销毁它）。

### 6.2 实测：没人 accept 过，Blink 照样回答 `send`

整段设计的地基是「一个从没被任何目的地 accept 过的类型，源端会不会回答」。真 Chromium 154，页面
`setData('text/plain','dnd-payload')`，`drag 150 100 150 900`：

```
nested: the pointer left the page mid-drag: taking text/plain;charset=utf-8 of 3 types
nested: the drag left the page as text/plain;charset=utf-8 (11 bytes, 3 types)
demo: drag out: text/plain;charset=utf-8, 11 bytes, types: text/plain;charset=utf-8|text/plain|chromium/x-web-custom-data
demo: drag out: taken, not carried
```

11 个字节，和请求在**同一毫秒**里回来（第一跑量到 3 ms）。页面那头的收尾是
`dragleave[...]@target 0,0|dragend[text/plain]@source 1010,663`，中间**没有** `drop`——载荷去了
宿主，不是页内某个角落。`target` 必须在 `send` 之前：没被告知要哪个类型的源，没有理由回答。

### 6.3 两个触发点，一条代码

- `nested_compositor_set_pointer_active (self, FALSE)`：GDK 说指针离开了 widget 时走的路。
- 坐标越界：`drag_motion()` 里拿 compositor 配置的 width/height 判。脚本喂的是坐标不是 crossing，
  这条存在的意义就是让自动化走到同一段代码。

两者都只在**有宿主听 `::drag-out`** 时动手（`g_signal_has_handler_pending`）。没听的宿主维持原样：
拖拽留着，后面松手照旧按 §2.4 取消。这就是为什么这功能上线后其余模式和门禁一条都没变。

### 6.4 宿主那条 GTK 腿只能人工测

demo 收到信号后做 `gdk_content_provider_new_for_bytes()`（图片则先写临时文件）+ `gdk_drag_begin()`，
一次只挂一条（新的移交先把旧的清掉），在 `drop-performed`/`dnd-finished`/`cancel` 里收尾。但
`gdk_drag_begin` 要的是**外层** compositor
上一次真实按下的 grab serial，脚本按下的是内层座位，GDK 拿不到那个 serial。所以自动跑一律
`CEFNEST_DRAG_OUT=take`：只预取，不把出去的那条 drag 真的举起来——举起来会落在人用的桌面上。

**2026-10-21 修正**：这条 GTK 腿此前把 `GTK_NATIVE(demo.view)` 当成了 surface，但 `ChromiumView` 只是
普通 `GtkWidget`，不是 `GtkNative`，`gdk_drag_begin()` 拿到的不是窗口 surface，真实拖出因此不会启动。
现在取 `demo.window` 的 native surface（toplevel），指针/display 也从同一个窗口来。自动门禁仍然走
`take`；真实拖出还是只能人工量。

**图片文件这条也补了**：Chromium 的图片 drag 实际提供的是

```
application/octet-stream;name="download.png"   74 bytes (the 8x8 test PNG)
```

而不是 `image/png`。`pick_drag_mime()` 现在优先选 `image/*` / `application/octet-stream`，文本 drag
仍优先选 `text/plain`。宿主把这份字节写进临时目录，用 `GdkFileList` 启动 `GdkDrag`，文件管理器
拿到的是真正的 `download.png` 文件（fixture：`test/image-dnd-page.html`）。临时文件在 `dnd-finished` 之后**不立即删**：实测文件管理器会在
`dnd-finished` 之后再打开 URI，太早删除就是“没有那个文件或目录”。它留到下一次 handover 或窗口关闭
时再清理。图片 path 不再走 `GdkContentProvider` 的 union（那个 file-list value 在 GTK 4.18 的 union
路径上会触发 `GDK_IS_CONTENT_PROVIDER` 断言），只提供文件列表。

GTK 4.18 的 `GdkDrag` 信号名也顺手修正了：旧代码连的是 `drop`/`failed`/`ended`，实际是
`drop-performed`/`dnd-finished`/`cancel`，旧名字连不上，拖拽收尾和临时文件清理都不会发生。
出界变成一次真 GTK 拖拽，得把手放到鼠标上量。`CEFNEST_DRAG_OUT=off` 干脆不连 handler，用来对照
「没有宿主」时页面的拖拽是什么样。

**这条怎么量**（人手一次，两分钟；只在真桌面上做，`test/ci-session.sh` 里外层 compositor
拿不到真实 grab serial，量不出这一条）：

1. 清空 `CEFNEST_DRAG_OUT`（默认就是"接住并举成宿主自己的 `GdkDrag`"），起
   `./builddir/src/cefnest-demo`，载入 `test/dnd-page.html`；
2. 把目标窗口（文件管理器或 gedit）摆在 demo **右边或下边**——路径上不许出现 GNOME 顶栏、
   左上角热区和左侧 dock，这条量的每一步都可能失手；
3. 真实鼠标从页面上的图按住、拖出 demo 窗口边界、进到目标窗口再松手；
4. 读三样：demo 的 trace 里那次预取的 mime 与字节数、目标窗口是否把内容落下来、
   `dnd-finished`/`cancel` 哪一个收尾。三条都对上，§6.4 才算测完，`test/dnd-gate.sh`
   的 `out` 那一段也才允许从 `take` 换成默认值。

同一次还能顺手量对面的腿（拖入）：从目标窗口往页面里拖，看 `drop-in` 那段的
`dragenter/drop` 是不是页面对象收到而不是宿主吞掉。

### 6.5 踩到的坑：绑 v3 的客户端必须接住 `target`

`seat-selftest-drag-out` 第一跑时 shm-client 被 signal 6 打死：

```
listener function for opcode 0 of wl_data_source is NULL
```

opcode 0 是 v3 的 `target`，而那个假客户端的 listener 表只填了 `send`/`cancelled`。libwayland 对
没有槽的事件直接 abort——和 `docs/empty-request-slot.md` 里 compositor 那半边是同一件事，只是这次
站在客户端。现在六个事件全填。反过来说这也是给我们自己的一个提醒：发 `target` 已经是对外走的一
步，任何绑 v3 的源端客户端都必须处理它。

### 6.6 假客户端重放的形状

`shm-client --drag-out` 在 `pointer_button` 的按下里发 `start_drag`（引的就是那次按下的 serial，
页面也是这么做的），offer 三个实测名字、`set_actions (COPY)`，被问到就答 `drag-out-payload`，
`cancelled` 时把自己收到过几次 motion、被读过几次、读的哪个类型打出来。它同时是自己拖拽的目的地，
但**不** accept、不 receive——要验的正是「目的地不存在」的那条路。

`seat-selftest-drag-out` 钉六件事：mime 是 `text/plain;charset=utf-8`；载荷 16 字节正好是
`drag-out-payload`；类型列表逐字符等于 `text/plain;charset=utf-8|text/plain|chromium/x-web-custom-data`；
移交之后 `nested_compositor_drag_get_types()` 回到空数组（页面的拖确实被收掉了）；fd 计数回到预取
之前（中继管道没漏）；两个触发点各跑一遍，期望的是累计第 1、第 2 次移交。把 §6.3 的越界判断改成一
条 `if (FALSE)` 跑一遍，两次 check 都会红并且报「the page kept its drag, still offering …」——这条
门禁是要能咬人的。

## 7. 第三段：宿主把拖拽带进来（`drag_from_host` + `::drag-in-end`）

### 7.1 方向反过来了：这一趟 compositor 是源

前两段源都是页面。第三段页面是目的地，源是 compositor 自己：
`nested_compositor_drag_from_host (self, mime_types, payload, can_move)` 把宿主手上的（类型表，字节）
交给 `data_source_host_new()`——那种 `DataSource` 后面没有客户端，载荷已经在内存里，类型表就是宿主
自己的 drop 报出来的那几个名字。之后全是 §1 那一套，只是主客对调：我们给指针下的 surface 介绍一个
`wl_data_offer`，页面自己 `accept`、自己 `set_actions`、自己发 `receive`。公开的入口是三样——这个
加上 `..._drag_from_host_drop()` 和 `..._cancel()`——加一个 `::drag-in-end (taken, mime)` 让宿主把
话答回它自己那条外层 `GdkDrop`。

| | 源 | 谁读载荷 | 这一趟由谁说完 |
| --- | --- | --- | --- |
| 出界（§6） | 页面 | compositor 自己预取 | compositor 发 `cancelled` |
| 拖入（本节） | compositor | 页面 | 页面发 `wl_data_offer.finish` |

源掩码恒定给 COPY|ASK，宿主说原件可以搬走（`can_move`）才再加 MOVE；demo 的 `drop-in` 传 FALSE。
实测的一轮协商是：源 `0x5`，页面 `set_actions (0x3, COPY)`，落到 `action 0x1`。

### 7.2 到达就得 `enter`，而且位置要先进

`wl_data_device.enter` 是 surface 得知「有拖拽」的唯一途径，而指针在拖入那一刻**已经**停在某个
surface 上了——没有人会再动一下，所以 `drag_from_host()` 末尾自己补一次 `drag_update_destination()`。
少了这一次，页面一个 `dragenter` 都不发，后面 motion 多少条都没用。demo 那边同一件事的另一半：
`drop-in` 先 `notify_pointer_motion (x0, y0)` 再 `drag_from_host()`，因为 compositor 只能按指针当前
位置挑要 enter 哪个 surface。

### 7.3 判定只能听目的地的话：`taken = dropped && concluded`

整段的地基问题是宿主该怎么回答它自己那条 `GdkDrop`。第一版按「目的地读过载荷」算，实测直接推翻。
`./test/dnd-gate.sh in` 四趟拖入（`DEMO_DROP_IN_TEXT` 是 `from-host`，9 字节，两个名字）：

```
drop-in 400 300 400 360   → #target   ：dragenter + 12×dragover + drop(from-host)，finish 也发了
drop-in 150 60 150 140    → #source   ：页面日志是空的；drop 照发，没人 finish
```

两趟在 compositor 这边都出现 `nested: handing the page 9 bytes of the host's drag`，而且**每趟两次**
（四趟共 8 次）；两次都发生在 `enter` 的那一毫秒里，`drop` 是 300 ms 之后的事：

```
05:57:41.934 handing the page 9 bytes …      ← 被 enter 到的那一刻
05:57:41.934 handing the page 9 bytes …
05:57:42.234 dropped text/plain as action 0x0
05:57:42.234 a drag ended without the destination finishing
demo: drop-in verdict: given back, text/plain
```

所以「读了」不等于「收了」，Blink 一被 `enter` 就把宿主的字节读两遍，跟它后面要不要这次 drop 无关。
能区分的只有 `finish`：落地**且**发过 finish 才算 `taken`。`struct _Drag` 里那个 `payload_read` 位
就是这么删掉的。同理，`nested: dropped … as action 0x1` 配 `a drag concluded` 是 taken，
`as action 0x0` 配 `a drag ended without the destination finishing` 不是。

一个 25 ms 的坑：motion 的头一拍里 Chromium 会先报一次 `settled on action 0x0 … destination 0x0`，
下一拍才落到 0x1。落地用的是**最后一次**协商结果，别按 enter 那条日志下结论。

取消那条路的 mime 是 `(none)`：`drag_cancel()` 会先 `drag_leave()`，而 leave 要把目的地的协商状态
整个擦掉（换 surface 得重新问过），`accepted_mime` 也在其中。反正 `taken == FALSE` 时宿主不需要
类型名，这个不修，但读 `::drag-in-end` 的人要知道它可能是空的。

### 7.4 并发与退休

一个座位只有一条拖拽。第二条进来时：

- 第一条还握着指针 → `nested: a drag already has the pointer, so the host cannot bring in a second`，
  返回 FALSE，旧的那条一动不动。
- 第一条已经落地但没 finish（§7.3 那种不接的页面）→ 先 `drag_destroy (…, FALSE)` 把它退休，
  日志是 `the drag that was dropped never said it was done`，然后新拖拽正常进来。`tell_host` 传
  FALSE 是刻意的：从「制造新拖拽」的那只手里去通知宿主旧判定，等于叫一个正在起手的人再拖第二条。

退休那条路径会把 host 的 `DataSource`（连同 GBytes）一起释放，所以第三条、第四条还能进来——门禁里
`in` 那一段跑的四趟就是这么排的，第四趟 `a fourth drag arrived after the other three ended`。

### 7.5 假客户端重放：`shm-client --drag-in` + `seat-selftest-drag-in`

这一段不需要浏览器也能钉死，用的是一趟真的 `wl_data_offer` 往返。`shm-client --drag-in` 是**主动**
的目的地：`enter` 里 `set_actions (COPY|MOVE, COPY)`、从 offer 的类型表里挑出 `text/plain`、
`accept` 掉，然后按页面的样子读两遍（一趟在 enter 之前，一趟在 drop 之后），字节不等于
`drag-in-payload` 就 `exit(1)`——`seat-selftest` 看见客户端非零退出就算红，判定不靠读日志。落地的
时候它发 `wl_data_offer_finish`。

`seat-selftest-drag-in` 排三趟，钉的是判定的三种收尾：taken（`text/plain`）、cancel 之后的
given back、退休后再进来一条仍然 taken。每趟 check 里还断言 `nested_compositor_drag_get_types()`
回到空数组（`the drag outlived its verdict`）和 fd 计数回到拖入之前。中间插一条 `STEP_DRAG_IN_SECOND`
专门撞 §7.4 的并发拒绝。反向做一遍负对照：把 `DRAG_IN_PAYLOAD` 改一个字符，客户端 exit 1，整条
`meson test` 红。

## 8. 还没有

- **拖入的宿主那一头已接（合成式）**：控件装 `GtkDropTarget`（文件列表 +
  字符串）。真实 drop 落下后才读出载荷（文件→uri-list、文本→text/plain），
  然后 `drag_from_host(mimes, payload)` 开嵌套拖、一次 motion、立刻
  `drag_from_host_drop` 收尾——整段嵌套拖压进真实 drop 的处理器里，与
  GTK 自己的拖拽状态机零并发。上一版在 enter 时就预取并开拖的设计与外层
  拖拽的活状态机相撞（`gdk_drop_status` 断言 + 窗口无响应），已废弃。
- **人工实测通过（2026-10-21）**：真实文本拖入 → 无卡死，页面 drop 处理器
  触发（标题 GOT-DROP）。
- 拖动图标目前是客户端给的 surface，宿主没有替换成 GTK 的拖拽图像。
- 越过窗口边界之后两条 GTK 腿都已接上并经人工拖文本实测（出界：编辑器收到 token；拖入：页面 drop 触发、无卡死）：`gdk_drag_begin` 用
  toplevel 的 native surface，文本走 `text/plain`，图片走临时文件 + `GdkFileList`（§6.4）。拖入那条
  对面的外层 `GdkDrop` 还没有 `GtkDropTarget`。两个都要 GDK 手里有一次真实的、它自己见过的按下，
  脚本给不了。
