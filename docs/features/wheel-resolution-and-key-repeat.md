# 滚轮分辨率与键盘自动重复：半格不能再当成 0，重复的键不能算按着两个

日期：2026-10-20
问题：任务 #24。滚轮那条路有四个毛病——步数截断、source 永远报 wheel、每个事件都当手势结束、
一次斜向滚头发两帧；键盘那条路是 GDK 把系统自动重复当作"又按了一次"递进来，我们的
`pressed_keys` 照单全收，于是客户端后来越来越相信有一把键正被按着。

> **#31 是这一篇的下半段**：#24 修完之后线级证据全对，但真页面的滚轮一动也不动。第 7 节记录
> 追到的原因（stop 和位移同帧）、三种发法的实测差别，以及这条不变式现在怎么进闸门的。

## 1. 旧代码在做什么

`src/chromium-view.c`（f724196 里的 `chromium_view_send_axis()`）：

```c
steps = (int32_t) delta;      /* 半格 0.5 → 0，分辨率在这一步就没了 */
value = delta * 10.0;
nested_compositor_notify_pointer_axis (self->compositor, axis, value, steps);
/* 两个轴 = 两次调用 */
```

`src/nested-seat.c`（旧 `nested_compositor_notify_pointer_axis()`）：

```c
wl_pointer_send_axis_source (sc->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);  /* 写死的 */
wl_pointer_send_axis (sc->pointer, time, axis, ...);
if (steps) wl_pointer_send_axis_discrete (sc->pointer, axis, steps);
wl_pointer_send_axis_stop (sc->pointer, time, axis);                      /* 每次都发 */
wl_pointer_send_frame (sc->pointer);
```

四条合起来的手感是：触摸板的连续滚动被说成有刻度的滚轮（Chromium 会往整格上吸），每个事件
都带 stop 所以客户端不敢接惯性，一次手势被拆成两次滚动，半格滚轮对老客户端等于没滚。

键盘那边旧代码是 `g_array_append_val (self->pressed_keys, evdev_code)` 无条件追加——一次重复
就多一条记录，而一次 `KEY_UP` 只 `g_array_remove_index` 一条。

## 2. 协议里只有 `axis_value120` 带得下分辨率

| 事件 | since | 能表达什么 |
| --- | --- | --- |
| `axis` | 1 | 表面像素（`wl_fixed`），连续量，谁都能收到 |
| `axis_source` | 5 | wheel / finger / continuous / wheel_tilt |
| `axis_discrete` | 5 | **整格**计数，半格只能是 0 |
| `axis_value120` | **8** | 1/120 格。spec 原话：它在"支持 version 8 的客户端里*取代* `axis_discrete`" |
| `axis_stop` | 5 | 手势结束，客户端可以从这里开始自己的惯性 |

量过的三个事实（不是推的）：

```
本机 wayland 1.23.1：WL_POINTER_AXIS_VALUE120_SINCE_VERSION = 8
wayland.xml：wl_seat / wl_pointer / wl_keyboard 都是 version="9"
CEF 154.0.34 的 helper 用版本 8 bind wl_seat，且它的 wl_pointer_listener 里 axis_value120 槽非空
```

`wl_pointer` 和 `wl_keyboard` 的版本**继承自客户端 bind `wl_seat` 时给的版本**，所以要让 value120
发得出去，先得把 seat 报到 8：`src/nested-internal.h:18`（同时 `NESTED_POINTER_VERSION`，
`src/nested-seat.c:18`）。

最后那句 helper 的测量是抬版本的前提。libwayland-**client** 对空的事件槽和 server 对空的请求槽
一样会 `wl_abort ("listener function for opcode %u of %s is NULL")`——这条字符串在两个库里各出现
一次（#19 量过），所以抬版本前不确认客户端填了槽，就是把 #19 的死法从请求方向搬到事件方向。
`test/shm-client.c:2311` 的注释记的就是这件事。

## 3. 修法

compositor 侧改成"一个设备的一次手势、两个轴一起、一帧"（`src/nested.h:100`）：

```c
void nested_compositor_notify_pointer_axis (NestedCompositor *self,
                                            double dx, double dy,
                                            int32_t value120_x, int32_t value120_y,
                                            uint32_t source, gboolean stop);
```

`src/nested-seat.c:710` 的 `axis_send_one()` 负责按版本降级：`axis` 永远发；`value120 == 0` 就到此
为止；版本 ≥8 发 `axis_value120`；5..7 只在 `value120 / 120 != 0` 时发 `axis_discrete`（协议里
discrete 只表达整格，硬凑一个 0 才是错）。`axis_stop` 只在 `stop` 为真时发。

widget 侧（`src/chromium-view.c:323`）把 GDK 的两个信息读出来，不再自己猜：

```c
GdkScrollUnit unit = gdk_scroll_event_get_unit (event);
gboolean stop      = gdk_scroll_event_is_stop (event);

if (unit == GDK_SCROLL_UNIT_WHEEL)
  {
    source     = WL_POINTER_AXIS_SOURCE_WHEEL;
    value120_x = lround (dx * 120.0);
    value120_y = lround (dy * 120.0);
    dx = value120_x / 12.0;   /* 一格 = 120 = 10 像素，两个通道说的是同一个数 */
    dy = value120_y / 12.0;
  }
```

`GDK_SCROLL_UNIT_SURFACE`（触摸板那种连续量）留 `source = FINGER`、`value120 = 0`，原样的像素距离
进 `axis`。注意 GTK 只有 `WHEEL`/`SURFACE` 两种单位，没有 CONTINUOUS 对应物。

脚本这边新加一步 `wheel <dx> <dy>`（`src/main.c:630`），单位是格，可以写 `wheel 0 0.5`，走的就是
widget 里同一套算术。

键盘：`src/nested-seat.c:809` 的 `pressed_keys` 去重——**已经是按下状态的键再次按下就是宿主的自动
重复，照发 `key` 给客户端（协议要求由 compositor 重复），但记账只记一次**。顺带把
`wl_keyboard.repeat_info` 从 `keyboard_send_enter()` 挪到 `seat_get_keyboard()`（`:515`），因为
协议说的是"自己重复按键的客户端要在第一次按下*之前*拿到速率"；GDK4 不再告诉 widget 系统设置，
用了惯例值 25/s、250 ms（`:22`）。识别重复只能靠"这个键已经按下"：GTK 4.18 的 GDK 头文件里没有
任何 `is_repeat`。

回归脚本 `test/seat-selftest.c:64` 加了这条：按 A → 再按一次 A（重复）→ 抬 A → `leave` +
重新 `enter`，让客户端报"我现在以为几个键按着"。

## 4. 量到的前后差别：重复键

同一条脚本，把去重临时关掉跑一次、还原以后再跑一次：

```
修复前：client: keyboard enter on the toplevel, serial=13, 1 keys held
修复后：client: keyboard enter on the toplevel, serial=13, 0 keys held
```

`1 keys held` 就是幽灵按住键：抬了一次 A，客户端却认为 A 还在按着，之后每个 `keydown` 都带着这个
状态。`WAYLAND_DEBUG=server` 也确认重复本身是透传了的，且 `repeat_info` 已经跑到任何按下之前：

```
[1820467.744] wl_seat#9.get_keyboard(new id wl_keyboard#22)
[1820467.810]  -> wl_keyboard#22.keymap(1, fd 49, 64753)
[1820467.821]  -> wl_keyboard#22.repeat_info(25, 250)      ← 第一次按下之前
...
[1821663.083]  -> wl_keyboard#22.key(6,  64689264, 30, 1)  ← KEY_A 按下
[1821725.072]  -> wl_keyboard#22.key(8,  64689326, 30, 1)  ← 同一键的重复，客户端要看
[1821785.230]  -> wl_keyboard#22.key(10, 64689386, 30, 0)  ← 抬一次
[1821905.623]  -> wl_keyboard#22.enter(13, wl_surface#3, array[0])   ← array[0]：没有键按着
```

## 5. 量到的前后差别：滚轮

`WAYLAND_DEBUG=server` 配上 `/tmp/wheel-half.script`（`wheel 0 1` / `wheel 0 0.5` /
`wheel 1 -0.25`），seat 8：

```
-> wl_pointer#21.axis_source(0)                      ← wheel
-> wl_pointer#21.axis(64725385, 0, 10.00000000)
-> wl_pointer#21.axis_value120(0, 120)
-> wl_pointer#21.axis_stop(64725385, 0)
-> wl_pointer#21.axis_stop(64725385, 1)
-> wl_pointer#21.frame()

-> wl_pointer#21.axis_source(0)
-> wl_pointer#21.axis(64725588, 0, 5.00000000)
-> wl_pointer#21.axis_value120(0, 60)                ← 半格有数了；旧代码这里是 discrete 0，干脆不发
-> wl_pointer#21.axis_stop(64725588, 0)
-> wl_pointer#21.axis_stop(64725588, 1)
-> wl_pointer#21.frame()

-> wl_pointer#21.axis_source(0)
-> wl_pointer#21.axis(64725788, 0, -2.50000000)
-> wl_pointer#21.axis_value120(0, -30)
-> wl_pointer#21.axis(64725788, 1, 10.00000000)
-> wl_pointer#21.axis_value120(1, 120)
-> wl_pointer#21.axis_stop(64725788, 0)
-> wl_pointer#21.axis_stop(64725788, 1)
-> wl_pointer#21.frame()                              ← 两个轴，只有一帧
```

> **这三段贴的形状后来被推翻了一半**：帧数是对的（一次手势一帧），但 stop 跟在位移同一帧里是
> 错的，而且错得很彻底——真 Chromium 收到这种形状**一点都不滚**。#24 那时候没有真页面可以对照，
> 线级看起来"讲得通"就收下了。根因、三种形状的实测差别和现在的断言在第 7 节。

降级路径不是纸面推理：把 `NESTED_SEAT_VERSION` 临时按回 5（客户端于是拿到 version 5 的 pointer），
跑同一条脚本：

```
整格：axis(..., 0, 10.00000000) + axis_discrete(0, 1)，没有 value120
半格：axis(..., 0, 5.00000000)，**步数一点都没有** —— 老客户端天生带不动分辨率
斜向：axis(…,0,-2.5) + axis(…,1,10.0) + axis_discrete(1, 1) + 两个 stop + 一个 frame
```

这两块合起来说明了关键点：单帧的修复跟版本无关（5 也修好了），分辨率的修复必须抬 seat。

## 6. stop 现在只发给动过的那根轴（2026-10-22 改，先前这里是「两个都发」）

**改前的取舍**：GDK 的手势结束事件本身不带距离（`dx = dy = 0`，只有 `is_stop` 为真），从这一个
事件里看不出刚才滚的是哪个轴，所以 `stop` 为真就给两个轴各发一条 `axis_stop`，并把"记住本次手势
动过哪些轴再挑着发"记成了可选优化。

**这条取舍是错的**（`docs/code-review-2026-10-22.md` **L9**）。wayland.xml 对 `axis_stop` 的说法是
"stop motion on the given axis"，它结束的是**这根轴上正在进行的那次滚动**；而滚轮只有一个方向，
于是每一格滚轮都附带一条"水平方向现在不在滚"——一条从没开始过的序列被告知结束了。Chromium
把手势序列记在 `ScrollController` 上，多余的 stop 会把刚建起来的序列提前结掉。

**改法**（`src/nested-seat.c`）：`SeatClient` 里加 `uint32_t scroll_axes` 位图，`axis` 发哪根就置
哪根；`stop` 帧只对置了的轴发 `axis_stop`，发完清零。指针离开 surface（`pointer_send_leave`）和
焦点清除（`seat_clear_focus`）时一并清零——不然下一根手指进来会带着上一根没滚完的账。

**线级形状（`seat-selftest` 默认脚本，现在跑三种手势）**：

```
client: axis 0 +100.00 (total +100.0) in frame #4          ← 只有竖直动
client: axis 0 stop (#1) in frame #5
client: axis-stop gate ok: stop frame #1 named exactly the axes 0x1 that moved
client: axis 1 +40.00 (total +40.0) in frame #6            ← 只有水平动
client: axis 1 stop (#2) in frame #7
client: axis-stop gate ok: stop frame #2 named exactly the axes 0x2 that moved
client: axis 0 +100.00 (total +200.0) in frame #8          ← 斜着滚：同一对帧里两根都动
client: axis 1 +40.00 (total +80.0) in frame #9
client: axis 0 stop (#3) in frame #10
client: axis 1 stop (#4) in frame #10
client: axis-stop gate ok: stop frame #3 named exactly the axes 0x3 that moved
```

判据在客户端（`test/shm-client.c` 的 `pointer_frame`）：把"stop 帧点名的轴集合"和"自上次 stop
以来真正动过的轴集合"比较，**不相等就 `exit (1)`**——比的是集合而不是固定答案，所以合成器想结束
哪种手势都由它决定，客户端只说"没人动过的轴不能被结束"、"动过的轴不能被漏掉"。
`client_exited()` 把它变成 `meson test` 的红。

反空跑也有一条：动了轴却 40 帧等不到 stop 帧，客户端同样 `exit (1)`，免得一次"没人被判定"的绿被
当成通过。每条判定还在成功时也打一行 `axis-stop gate ok:`，绿跑自己带着"判了几次"。

**变异（把两条 stop 改回无条件各发一条）**：

```
client: 1963: axis-stop gate FAILED: the stop frame named axes 0x3 but only 0x1 ever sent an axis event, so the compositor ended a scroll sequence that never started
selftest: the client exited with status 1
```

`meson test --suite selftest`（当时 21 条：20 个 seat 模式 + `map-cycle`）里 **16 条红**，改回来
21/21 绿。剩下 5 条报不出来，原因值得记：`map-cycle` 不碰 seat；`--refused`、`--pool-budget`、
`--pool-shrink`、`--second-window` 这四条**本来就指望客户端自己提前挂断**，客户端的退出码在那几条
里不是判据（`client_exited()` 先看 `run_finished`），所以这条断言在它们身上是盲区——它们的绿不能
当成"stop 形状对"的证据，16 条才是覆盖面。

> 第 7.5 节贴的那段日志写于本节改前，里面的 `axis 0 stop (#1)` + `axis 1 stop (#2)` 是"两轴无条件
> 各一条"的形状；现在同一格竖直滚轮只发 `axis 0 stop`，两轴的那一行出现在斜向手势里。7.5 的不变式
> （stop 不能挤在位移那一帧）本身没变，判据也没变。

## 7. #31：stop 挤在位移那一帧里，Chromium 把这一步整个丢掉

第 5 节的线级形状说得通，接到真页面上才发现**滚轮根本没滚**。这一节是把它追到底追出来的东西。

### 7.1 症状

`test/wheel.session` 打开 `test/wheel-page.html`（三十行 100 px 的长页），`hover` 到页面中间，
`wheel 0 3`，然后读 `document.scrollingElement.scrollTop`：

```
demo: loaded file:///…/test/wheel-page.html (HTTP 200)
demo: script 8 ok value="0,"        ← 后面两次分别也是 "0," 和 "0,"
```

不只是"滚得少"。`window.__w` 是空数组，页面自己注册的 `wheel` 监听一条都没收到——Blink 这边
**连一个 wheel 事件都没派发过**，等于输入没到过。

### 7.2 根因

就是第 5 节那个"看起来对"的形状：`axis_stop` 和它要结束的那条 `axis` 发在同一帧里。

```
-> axis_source(0) -> axis(…, 0, 100.0) -> axis_value120(0, 1200)
-> axis_stop(…, 0) -> axis_stop(…, 1) -> frame()      ← 这一帧的位移被丢掉
```

要说清楚责任在哪：`wayland.xml` 只规定 stop *之后*同轴的 `axis` 算新序列（"Any wl_pointer.axis
events with the same axis_source after this event should be considered as the start of a new axis
motion"），**没要求把 stop 之前的位移丢点什么**。是 Chromium 的实现收到 stop 就把这次序列结算
掉，于是同一帧累积的位移不会变成 wheel 事件。协议没错、我们也没违规，但**发法在真页面上是坏的**，
所以得改：stop 单独占一帧（`src/nested-seat.c:773`）。位移和 stop 一起来时先发位移帧、再发只有
stop 的那一帧——这正好是真实设备的形状。

### 7.3 三种形状在真 Chromium 上的差别（都实测了）

同一条 `test/wheel.session`，只改 stop 放哪里：

| stop 怎么发 | `wheel 0 3` 后的 scrollTop | DOM 里的 deltaY 序列 |
| --- | --- | --- |
| 和位移同一帧（#24 的形状） | **0** | 空 |
| 单独一帧紧跟位移（协议允许，但刻度不该带 stop） | **387.2** | `360` 之后 15 条：`0.74 8.35 8.54 4.56 5.95 5.27 4.64 4.26 3.25 2.91 2.37 1.84 1.33 0.82 0.32` |
| 滚轮刻度不带 stop（现在的做法） | **360** | `360` |

中间那一行才是决定性的：**挂上 stop，位移确实送到了，但 Chromium 把它当成一次手势的结束，
从 stop 那里起自己补了一段收尾动画**——页面多滚 27.2 px，DOM 里多出 15 条谁也没按出来的衰减
事件。真实滚轮不会这样，因为 GDK 那边就从不把一次刻度标成 stop。`~/deps-src/gtk-4.24.1` 的
`gdk/wayland/gdkseat-wayland.c:604` `flush_scroll_event()` 写得很清楚（本机装的是 4.18.6，读的
是 4.24.1 的源码，4.18.6 是不是同一份逻辑没去核对；真人手上的滚轮也还没量过，见第 9 节）：

```c
if (pointer_frame->value120_x || pointer_frame->value120_y)   /* 有刻度 = 滚轮 */
  flush_discrete_scroll_event (…);                            /* 这条路径根本不发 stop */
else if (pointer_frame->is_scroll_stop || delta_x || delta_y)
  {
    if (pointer_frame->is_scroll_stop && delta_x == 0 && delta_y == 0)
      is_stop = TRUE;                                         /* 只有"零距离 + 结束"才算 */
```

所以做法照 GDK 的形状来：`src/main.c:658` 的 `wheel` 命令和 `test/seat-selftest.c:489` 的 axis 步
都传 `FALSE`，只有抬指传 `TRUE`。协议也明说这是合法的：

```
If the source is wl_pointer.axis_source.wheel, … a wl_pointer.axis_stop event may
or may not be sent. … clients must not rely on receiving an axis_stop event for
these scroll sources and should treat scroll sequences from these scroll sources
as unterminated by default.
```

（`finger` 那句是反过来的："a wl_pointer.axis_stop event **will** be sent when the user lifts the
finger off the device"——所以 `STEP_AXIS_STOP` 那一步用的是 FINGER 源、零距离。）

分辨率那一环顺带有了数字：**Chromium 把一个刻度算成 120 CSS px**，跟我们表面像素通道里的
10 px/刻度无关。实测 `wheel 0 3` → 360，`wheel 0 0.5` → 420（半格 +60），`wheel 0 5` → 1020；
DOM 里 `deltaY` 是 `360|60|600`，`deltaMode` 是 0（像素）。两个通道（`axis` 浮点和
`axis_value120`）都能驱动滚动——只要 stop 不错位。

### 7.4 JS 侧 `wheel` 监听：丢的是注册之后的那一段手势

`window.onwheel` 不触发这件事跟帧无关，得单独记账。三组对照（都是真 Chromium，都是 `wheel 0 1`）：

| 做法 | 第一次手势 | 之后 |
| --- | --- | --- |
| 注册后**立刻**滚，手势之间隔 500 ms | `__w` 空，但 `scrollTop` 照样 +120 | 第 2、3 次还是空 |
| 注册后 `wait 1500` 再滚 | `__w = 120`，`scrollTop` 120 | 每次都收到（240、`120|120`） |
| 注册后立刻滚（丢），停 3 s 再滚 | 丢 | 收到；接下来 1 s 间隔也继续收到 |

**页面每一组都滚对了 120 px**，差的只是要不要回调 JS：Blink 还没把"这页有 wheel 监听"这件事交给
合成器输入路径时，滚动走的是不回调 JS 的快速路径，而且这段序列还没结束前（500 ms 连着滚）后面
几次照旧走快速路径。我们发的字节是同一份，所以这不是 seat 的账。`test/wheel.session` 里注册监听
之后那句 `wait 1500` 就是给 Blink 留的，不是给 compositor 留的。

### 7.5 这条不变式现在有人守

分帧这件事光看日志看不见，所以 `test/shm-client.c` 把 `wl_pointer.frame` 编号打出来，并且把它
变成断言（`test/shm-client.c:1391`）：

```
client: axis 0 +100.00 (total +100.0) in frame #4
client: axis 0 value120 +1200 (total +1200) in frame #4
selftest: axis-stop
client: axis source finger, frame #5
client: axis 0 stop (#1) in frame #5
client: axis 1 stop (#2) in frame #5
```

同一帧里同时出现某轴的 `axis` 和该轴的 `axis_stop`，客户端打一行说明然后 `exit (1)`；
`test/seat-selftest.c:136` 的 `client_exited()` 等的就是它：客户端以非零状态或信号离开，run 立刻
停在那一步、退出码 1，`meson test` 直接红。这是"事件形状"第一次进了闸门——以前客户端半路死了
没人管。提前 `exit (0)` 不算抱怨（`--refused` 和收到 `xdg_toplevel.close` 都是这么收尾的），所以
只认非零；问一个还活着的子进程要状态会是 GLib critical，所以是 `g_subprocess_wait_async()` 等它，
不是轮询。

把 stop 临时挪回位移那一帧、axis 步改回 `TRUE`，手跑一次（fd 基线跟 `meson test` 不同）：

```
client: axis 0 stop came in frame #4, the same frame as the axis event it stops
selftest: the client exited with status 1
selftest: finished at step 7, 139 fds open (127 at start)     ← 后面 22 步没跑
exit=1
```

## 8. 验证

| 跑法 | 结果 |
| --- | --- |
| `meson test --suite selftest` 七模式（plain / `--dmabuf` / `--text-input` / `--cursor-shape` / `--popup` / `--refused` / `--dnd-requests`） | 全部 `finished at step 27`；fd **净增 18**（`119 → 137` 这一轮，`--refused` 是 `119 → 132`，净增 13）——脚本多了 3 步但一个 fd 都没多漏，#19 那轮的净增数也是 18；五个能收输入的跑法都打出 `axis source wheel`、`axis 0 value120 +1200`、`stop (#1)(#2)`、`0 keys held` |
| 同一脚本、把 `pressed_keys` 去重临时关掉 | 同一位置报 `1 keys held`（第 4 节） |
| `meson test --suite selftest` 八模式（#32 之后多出 `--gtk-client`，闸门共 9 条） | 全部 OK、`finished at step 29`；净增 fd 与上一行同一模式一致（plain 17、`--refused` 12、`--dmabuf` 31），`--gtk-client` 那条自己再启一个 GTK 客户端也还是 17 |
| `WAYLAND_DEBUG=server` | 第 4、5 节贴的块，一帧一次手势 |
| `NESTED_SEAT_VERSION` 临时按回 5 | 降级路径按预期发 `axis_discrete`、半格无步数 |
| `meson test --suite protocol`（`python3 test/audit-impl-slots.py builddir`） | `0 uncovered slot(s), 0 unknown interface(s), 29 implementation struct(s)`——抬 seat 版本以后仍是 0 |
| 真 Chromium（`test/wheel.session` + `test/wheel-page.html`） | **跑成了**：`scrollTop` 360 → 420 → 1020，DOM `deltaY` 是 `360\|60\|600`（第 7.3 节）；把 stop 挪回同一帧作对照，三个数全是 0（第 7.1 节） |
| 客户端断言（临时把 stop 挪回位移帧） | `the client exited with status 1`、run 停在第 7 步、退出码 1（第 7.5 节） |

`shm-client` 也补了 `axis_source` / `axis_stop` / `axis_value120` 三个监听（`test/shm-client.c:1429`
的 `pointer_listener`），否则它一 bind 到 8 就会因为空事件槽被 libwayland-client 打死——这正是第 2
节要防的事。

## 9. 还没做的

- **真人手上的滚轮/触摸板没验**：`unit` 和 `is_stop` 两个映射只在脚本路径上测过，脚本喂的是合成
  事件。第 7.3 节的三种形状是脚本 `wheel` 命令造出来的，触摸板那条（FINGER 源 + 零距离的
  `STEP_AXIS_STOP`）只有线级证据，没在真 Chromium 上验过连续滚动手感。
- **飞滚（fling）惯性**：`axis_stop` 的时机对了，但"手指离开后 Chromium/客户端自己接住动量"这件事
  反过来了——**给滚轮刻度多发一个 stop，Chromium 会自己补一段 27.2 px 的收尾动画**（第 7.3 节），
  所以真正的触摸板连续滚到底该给什么形状（连续位移 + 最后一次 stop）还没在真页面上量过。
- **`--gtk-client` 只守 GTK 的初始化时机**：它证明"开显示那一下的事件序列能活下来"，不证明滚动
  手感。滚轮形状的账现在由 shm-client 的断言守（第 7.5 节），但那只覆盖 `axis`/`axis_stop` 一条。
- **重复速率是硬编码**：25/s、250 ms。GTK4 不再把这套设置交给 widget，真要跟随系统得从
  `org.gnome.desktop.peripherals.keyboard`（dconf 或 portal）读。
