# 客户端还在 gtk_init 里，就不能把选择塞给它

## 1. 症状

helper 一起来就死，0.3 秒内 signal 6，页面根本不 load：

```
**
Gdk:ERROR:../../../gdk/gdk.c:490:gdk_display_should_use_portal: assertion failed: (display != NULL)
Bail out! Gdk:ERROR:../../../gdk/gdk.c:490:gdk_display_should_use_portal: assertion failed: (display != NULL)
** Message: session: helper killed by signal 6
```

这条在 `docs/wheel-resolution-and-key-repeat.md` 第 8 节里被记成"环境问题"，并把 #24 的最后一环
（真 Chromium 的滚轮复测）压到了后面。它不是环境问题，是我们 compositor 的问题。

## 2. 一个假的相关变量

第一次能看出规律：**外层 demo 跑在 X11 上时必崩，跑在 Wayland 上时不崩**（`GDK_BACKEND=x11` 是这台
机器宿主环境里就有的）。于是先做了两件事：

- `src/chromium-session.c:575` 给 helper 强制 `GDK_BACKEND=wayland`。理由本身就成立：我们只替它
  取消了 `DISPLAY`，而 GTK 读 `GDK_BACKEND` 比读任何 display 变量都早，宿主留一个 `x11` 进来它就
  去开一个已经被我们抽掉的 X11 display。
- `helper/src/main.cpp:741` 的启动自检把 `GDK_BACKEND` 也打出来。

两条都留下，但**崩溃照旧**。接着依次试掉了一串看起来都很像的假设，每个都实测：

| 试的东西 | 命令/做法 | 结果 |
| --- | --- | --- |
| 宿主的 GTK 模块漏进 helper | `GTK_MODULES` 取消、`NO_AT_BRIDGE=1`、`GTK_A11Y=none` | 三次都崩 |
| 走 xdg-desktop-portal 拿设置 | `GDK_DEBUG=no-portals`、取消 `DBUS_SESSION_BUS_ADDRESS` | 都崩 |
| offer 里的 X11 Atom 名（`TEXT`/`STRING`/`UTF8_STRING`）只留两个真 mime | 改 `src/nested-data-device.c:417` | 都崩 |
| 新客户端的初始回放整个关掉（只掏空 `compositor_advertise_selection`） | 掏空 | 还崩——漏了 `get_data_device` 里那条直连 `data_device_advertise` 的路，这条当时测了个假的阴性 |
| 是不是客户端起太早 | helper 前先 `sleep 2` | 还崩 |
| 是不是某个 global 不该广告 | 逐个藏 `wl_data_device_manager` / text-input / cursor-shape / viewporter+fractional / dmabuf | 都崩；藏掉 data-device-manager 之后 GDK 直接开不了 display |

## 3. 把 CEF 从等式里拿掉

真正让这件事变快的是二十行的 throwaway：一个普通 GTK4 程序，`gtk_init_check()`、开个窗口、1 秒后读
一次剪贴板、3 秒退出。让它连我们已经在跑的嵌套 compositor：

```
WAYLAND_DISPLAY=<demo 打出来的 socket> GDK_BACKEND=wayland DISPLAY= ./gtk4-client
```

同一个断言，同一个 exit 134——**跟 Chromium 一点关系都没有**，而且一轮只要 2 秒，不是 20 秒。

然后 `gdb -batch -ex run -ex bt` 给出栈（`#8` 那个导出符号只是最近的邻居，真正的函数是库里的局部
函数）：

```
#4  g_assertion_message_expr
#5..#9 libgtk-4            ← 处理 data offer / clipboard
#13,#14 libwayland-client   ← 事件派发
#15 wl_display_dispatch_queue_pending
#16 libgtk-4
#17 gdk_display_manager_open_display
#18 gtk_init_check
#19 gtk::GtkInitCheck(int*, char**) at ../../ui/gtk/gtk_compat.cc:66
```

`GDK_DEBUG=clipboard` 把崩之前最后两行说清楚了：

```
data device data offer, data device 0x5625cd1782d0, offer 0x5625cd151b40
0x5625cd144be0: remote clipboard claim for text/plain;charset=utf-8 ... STRING
** Gdk:ERROR: ... assertion failed: (display != NULL)
```

## 4. 机制

客户端在 `get_data_device` 之后立刻做一次 roundtrip（GTK 的 display open 里）。我们为了"晚来的客户
端也能粘贴"，在处理这个请求的当场就把当前选择回放给它：`data_offer` + 一串 `offer` +
`source_actions` + `selection`，全都排在同一条 roundtrip 的回复里。

GDK 收到 `selection` 就为这个 offer 建 clipboard 对象，而这个对象要问 GDK 现在的 display 是哪个。
default display 要等 `gdk_display_open()` **返回之后**才设得上，此刻它还是 NULL，`gdk.c:490` 直接
`g_assert` 掉整个进程。

也就是说：我们发的是完全合法的事件，只是发在了客户端唯一还不能收它的时刻。

至于"外层 X11 必崩、外层 Wayland 不崩"——那是**缓存冷热**，不是后端。第 2 节那张表后面又做了一次
对照：把修复前的代码拿去连"外层 Wayland"的 compositor，四个客户端里崩了三个。第一个客户端自己触发
镜像读取（`compositor_paste_clipboard`），读回来是异步的，落在 init 之后，于是活；后面每个客户端连上
时 `clipboard_bytes` 已经缓存好了，我们同步就答，于是崩。宿主剪贴板里有没有文本决定这件事看起来
"突然坏了"还是"一直好的"——这台机器上文本是这几天才一直留着的。

## 5. 修复

规则只有一条：**选择的通知不从客户端请求的派发里发出去**，一律排队到下一次循环。

- `src/nested-data-device.c:492` `compositor_advertise_selection()` 现在只登记一个 idle；
- `src/nested-data-device.c:466` `compositor_replay_selection()` 在 idle 里遍历当前还活着的
  `data_devices`，把每个还没有 offer 的设备按 `self->selection_source` 的**当时**状态回放；
- `src/nested-data-device.c:873` `get_data_device` 不再直接调 `data_device_advertise`；
- `src/nested-internal.h:211` 加 `selection_replay_id`，`src/nested.c:1737` 在 dispose 里取消它。

为什么一个 idle 就够：客户端的 `get_data_device` 和紧随其后的 `wl_display.sync` 在同一批请求里，这一
批派发完才会有那条 idle——我们的事件于是排在 sync 回复**后面**。`wl_display_roundtrip` 只处理到 sync
回调为止，剩下的事件要等客户端下一次派发，那时 `gtk_init` 已经返回，default display 设上了。

参数从 `(self, source)` 变成 `(self)`：排队意味着不能带着一个可能在我们发出去之前就被销毁的
`DataSource`。原来的八个调用点要么刚把 `self->selection_source` 设成这个 source，要么在调用前已经把它
清成 NULL 并做了判断，所以"发的时候读当前状态"和原来等价，还顺便把同一批里多次变化合成一次。

## 6. 验证

| 跑法 | 结果 |
| --- | --- |
| 修复前，外层 X11 + 普通 GTK4 客户端 × 3 | 3/3 `Bail out`，rc=134 |
| 修复前，外层 Wayland + 普通 GTK4 客户端 × 4 | 1 活 3 崩（第 4 节） |
| 修复后，外层 X11 + 客户端 × 3 | 3/3 `opened display`，rc=0 |
| 修复后，外层 Wayland + 客户端 × 5 | 5/5 rc=0，且每个都打印 `clipboard holds 8 bytes`——晚来的客户端照样能粘贴 |
| 宿主 → 嵌套 compositor → GTK4 客户端 的粘贴 | `the widget clipboard holds 8 bytes of text` → `offered the selection as id ...` → 客户端 `clipboard holds 8 bytes: Electron` |
| helper（外层仍是宿主的 `GDK_BACKEND=x11`） | `helper ready` → `browser created` → `window created` → `loaded ... (HTTP 200)` → `helper stopped (stopped by the host)`，没有 `Bail out` |
| `meson test -C builddir seat-selftest-gtk-client`（修复前的代码） | FAIL：子进程打印 `Gdk:ERROR:...gdk_display_should_use_portal: assertion failed: (display != NULL)` + `Bail out!`，父进程报 `gtk4 client killed by signal 6`，退出码 1 |
| 同一条（修复后） | OK：子进程 `opened display 'wayland-1'` → `clipboard holds 'text from the nested client'`，父进程 `gtk4 client survived the events of its own display open` |
| `meson test -C builddir` | 9/9 OK，含 `--dnd-requests`（那条正是靠回放把 offer 送给第二个连接） |

守卫就是上面那两行的差别，它长这样：`--gtk-client` 让 seat-selftest 在脚本走完之后，把**自己**再
启动一遍（`/proc/self/exe --as-gtk-client`），环境变量只有我们那个 socket——`WAYLAND_DISPLAY` 换成
嵌套 socket、`GDK_BACKEND=wayland`、`DISPLAY` 抹掉，所以这个客户端不可能连到宿主上去。子进程只做三
件事：`gtk_init_check()`（崩就崩在这里）、`gdk_clipboard_read_text_async()`、跑 1.2 秒主循环。offer 的
来源是第一个客户端的 `--copy`（seat-selftest 在这个模式下会自动带上它），不依赖宿主机剪贴板里当时有
没有东西——不然这条守卫在别的机器上就变成一次空跑。父进程等到子进程报告才结束，所以 `Bail out` 一定
变成 FAIL 而不是碰巧的 OK。

## 7. 还没做的

- **只守住了 GTK**。这个 bug 的类别——事件时机把真 toolkit 打死——现在由
  `seat-selftest --gtk-client` 守（第 6 节），但它守的是 GTK4 在 init 里要 `wl_data_device` 这一条路
  径；Qt / SDL / Electron 客户端在 init 里收到 selection 会不会有类似问题，没测，仓库里也没有它们的客
  户端。
- **GDK 是 4.18.6**。这个 `g_assert` 后来有没有被 GTK 自己改掉、改掉以后是不是就安全，没查；我们的
  排队规则不依赖它改不改。
- **滚轮没跟真 Chromium 复测成功**：helper 现在起来了，`test/wheel.session` 的
  `document.scrollingElement.scrollTop` 在 `wheel 0 3` / `0 0.5` / `0 5` 之后**还是 0**。那是 #31。
