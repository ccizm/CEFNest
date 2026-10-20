# 滚轮分辨率与键盘自动重复：半格不能再当成 0，重复的键不能算按着两个

日期：2026-10-20
问题：任务 #24。滚轮那条路有四个毛病——步数截断、source 永远报 wheel、每个事件都当手势结束、
一次斜向滚头发两帧；键盘那条路是 GDK 把系统自动重复当作"又按了一次"递进来，我们的
`pressed_keys` 照单全收，于是客户端后来越来越相信有一把键正被按着。

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
`test/shm-client.c:2112` 的注释记的就是这件事。

## 3. 修法

compositor 侧改成"一个设备的一次手势、两个轴一起、一帧"（`src/nested.h:98`）：

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

键盘：`src/nested-seat.c:804` 的 `pressed_keys` 去重——**已经是按下状态的键再次按下就是宿主的自动
重复，照发 `key` 给客户端（协议要求由 compositor 重复），但记账只记一次**。顺带把
`wl_keyboard.repeat_info` 从 `keyboard_send_enter()` 挪到 `seat_get_keyboard()`（`:515`），因为
协议说的是"自己重复按键的客户端要在第一次按下*之前*拿到速率"；GDK4 不再告诉 widget 系统设置，
用了惯例值 25/s、250 ms（`:22`）。识别重复只能靠"这个键已经按下"：GTK 4.18 的 GDK 头文件里没有
任何 `is_repeat`。

回归脚本 `test/seat-selftest.c:62` 加了这条：按 A → 再按一次 A（重复）→ 抬 A → `leave` +
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

降级路径不是纸面推理：把 `NESTED_SEAT_VERSION` 临时按回 5（客户端于是拿到 version 5 的 pointer），
跑同一条脚本：

```
整格：axis(..., 0, 10.00000000) + axis_discrete(0, 1)，没有 value120
半格：axis(..., 0, 5.00000000)，**步数一点都没有** —— 老客户端天生带不动分辨率
斜向：axis(…,0,-2.5) + axis(…,1,10.0) + axis_discrete(1, 1) + 两个 stop + 一个 frame
```

这两块合起来说明了关键点：单帧的修复跟版本无关（5 也修好了），分辨率的修复必须抬 seat。

## 6. 一个刻意的取舍：stop 时两个轴都发

GDK 的手势结束事件**本身不带距离**（`dx = dy = 0`，只有 `is_stop` 为真），从这一个事件里看不出刚
才滚的是哪个轴。所以现在 `stop` 为真就给两个轴各发一条 `axis_stop`。协议没禁止给没动过的轴发
stop，客户端收到的是"这个方向现在不在滚"这个陈述，不会多出位移。要更讲究就得在 `SeatClient` 里
记住本次手势动过哪些轴再挑着发——留作可选。

## 7. 验证

| 跑法 | 结果 |
| --- | --- |
| `meson test --suite selftest` 七模式（plain / `--dmabuf` / `--text-input` / `--cursor-shape` / `--popup` / `--refused` / `--dnd-requests`） | 全部 `finished at step 27`；fd **净增 18**（`119 → 137` 这一轮，`--refused` 是 `119 → 132`，净增 13）——脚本多了 3 步但一个 fd 都没多漏，#19 那轮的净增数也是 18；五个能收输入的跑法都打出 `axis source wheel`、`axis 0 value120 +1200`、`stop (#1)(#2)`、`0 keys held` |
| 同一脚本、把 `pressed_keys` 去重临时关掉 | 同一位置报 `1 keys held`（第 4 节） |
| `meson test --suite selftest` 八模式（#32 之后多出 `--gtk-client`，闸门共 9 条） | 全部 OK、`finished at step 28`；净增 fd 与上一行同一模式一致（plain 17、`--refused` 12、`--dmabuf` 31），`--gtk-client` 那条自己再启一个 GTK 客户端也还是 17 |
| `WAYLAND_DEBUG=server` | 第 4、5 节贴的块，一帧一次手势 |
| `NESTED_SEAT_VERSION` 临时按回 5 | 降级路径按预期发 `axis_discrete`、半格无步数 |
| `meson test --suite protocol`（`python3 test/audit-impl-slots.py builddir`） | `0 uncovered slot(s), 0 unknown interface(s), 29 implementation struct(s)`——抬 seat 版本以后仍是 0 |
| 真 Chromium（`test/wheel.session` + `test/wheel-page.html`） | **这次没跑成**，见第 8 节 |

`shm-client` 也补了 `axis_source` / `axis_stop` / `axis_value120` 三个监听（`test/shm-client.c:1368`
等），否则它一 bind 到 8 就会因为空事件槽被 libwayland-client 打死——这正是第 2 节要防的事。

## 8. 还没做的

- **Blink 那一侧要复测**。#24 的最后一环本来是"真的把 Chromium 的视口滚起来"，这一次被 helper 自己
  挡住了：它一起来就撞 `gdk.c:490 gdk_display_should_use_portal: assertion failed (display != NULL)`
  （signal 6），0.3 秒内死掉，页面根本不 load。当时做了两次对照确认不是这次改动带出来的——跑**完全
  没改过的 `test/drag.session`**（#19 里 3/3 通过），以及把 `NESTED_SEAT_VERSION` 按回 5 重建后再跑，
  两次都是同一行断言——所以当时把它记成了"环境/ helper 自己的问题"。

  **它不是环境问题，是我们的 compositor 发事件发错了时机**，根因、排除掉的假设和修复都在
  `docs/clipboard-offer-during-client-init.md`。helper 现在在外层 X11 的宿主环境里也能起来了，复跑
  上面那条命令的实际结果是：`browser created` → `loaded ... (HTTP 200)` 全程没有 `Bail out`，但
  `document.scrollingElement.scrollTop` 在 `wheel 0 3`、`wheel 0 0.5`、`wheel 0 5` 之后**仍然是 0**
  （`script 3/6/9/12 ok value=0`）。线级证据（第 5 节）说明我们发出去的东西是对的，所以问题在更后面
  的一环，那是 #31。
- **真人手上的滚轮/触摸板没验**：`unit` 和 `is_stop` 两个映射只在脚本路径上测过，脚本喂的是合成
  事件。
- **JS 侧 `wheel` 监听对不上**：之前那次成功的 CEF 跑里视口确实滚了，但页面自己注册的
  `window.onwheel` 一次都没触发。当时没追下去，需要复现后判断是我们少发了什么还是页面写法问题。
- **飞滚（fling）惯性**：`stop` 现在是对的，但客户端在 stop 之后接住动量、触摸板连续滚是否顺滑，
  要等 CEF 复测。
- **重复速率是硬编码**：25/s、250 ms。GTK4 不再把这套设置交给 widget，真要跟随系统得从
  `org.gnome.desktop.peripherals.keyboard`（dconf 或 portal）读。
