# 空请求槽：把 data_device_manager 报到版本 3，compositor 就替客户端按下了自毁键

日期：2026-10-20
问题：任务 #19。之前偶尔有一跑是这样收尾的——demo 进程 134 退出，日志最后一行是
`listener function for opcode 2 of wl_data_source is NULL`，紧跟一句
`Lost connection to Wayland compositor`。要把它复现出来并且说清楚为什么。

## 1. 现场长什么样

`/tmp/host4.log`（6 次跑里唯一中招的那次，第 85–87 行）：

```
nested-DEBUG: 12:30:45.370: nested: pointer becomes grabbing
listener function for opcode 2 of wl_data_source is NULL
Gdk-Message: 12:30:45.376: Lost connection to Wayland compositor.
```

三行贴在一起，中间没有任何别的输出。`host1..3,5,6.log` 里 `is NULL` 一次都没有，
所以它看起来像"跑十次撞一次"的偶发问题。

## 2. 两次把方向判断反了

第一反应是：`wl_data_source` 是 compositor 发给客户端的事件，客户端（GDK/ozone）没实现
`.cancelled` 所以挂了。这个说法有两处错，都是量出来的：

- **这句话两个库里都有。** 光凭文字分不出是请求方向还是事件方向：

  ```
  $ for f in libwayland-client.so.0 libwayland-server.so.0; do
      printf '%s: ' $f; grep -ac 'listener function for opcode' /usr/lib/x86_64-linux-gnu/$f; done
  libwayland-client.so.0: 1
  libwayland-server.so.0: 1
  ```

- **core dump 的栈帧在 server 侧。** `ulimit -c unlimited` 之后抓到的现场，#4–#6 帧落在
  libwayland-server 里，也就是**服务端在派发一个客户端请求**时发现槽是空的。挂掉的是我们
  自己的 `data_source_impl`，不是 Chromium 的监听器。

顺带两条踩坑记录：

- **gdb 在这台机器上是个海森堡机器**：8 次挂在 gdb 底下都不复现，helper 单跑还撞出另一个
  `gdk_display_should_use_portal` 断言。要看时序只能回到 core dump。
- core 落在进程 CWD（`core_pattern=core`），不要在仓库目录里跑，`/tmp/cores` 那种地方比较干净。

## 3. opcode 的真值表（按装好的头文件核对）

`wl_resource` 的 implementation struct 是**按 wire opcode 索引的数组**，libwayland-server 把
空槽当成"我自己的 bug"而不是"这个客户端不该发这个请求"，于是直接 `wl_abort`：

| 接口 | 请求（客户端→compositor） | 事件（compositor→客户端） |
| --- | --- | --- |
| `wl_data_source` | offer 0、destroy 1、**set_actions 2** | target 0、send 1、cancelled 2、dnd_drop_performed 3、dnd_finished 4、action 5 |
| `wl_data_offer` | accept 0、receive 1、destroy 2、**finish 3**、**set_actions 4** | source_actions 0、action 1 |

`set_actions`（source）和 `finish` / `set_actions`（offer）都是 **version 3 才有的请求**，
而 `src/nested-data-device.c:26` 把 `NESTED_DATA_DEVICE_MANAGER_VERSION` 报成了 3。也就是说：
我们一边对外承诺"我支持 v3"，一边把这三个槽留空。

## 4. 谁发的：一次真的文字拖拽

`test/drag-page.html` + `test/drag.session`（脚本语法新加了 `drag <x0> <y0> <x1> <y1> [button]`，
按下以后按 12 步、每步 25 ms 移动，因为 Chromium 是按"按住移动了多远"决定要不要开拖拽的，
一步跳过去不算拖拽）在**修复前**的构造上：

```
$ GDK_BACKEND=wayland G_MESSAGES_DEBUG=nested \
    CHROMIUM_HELPER=$PWD/helper/build/Release/chromium-helper \
    CHROMIUM_URL=file://$PWD/test/drag-page.html \
    CHROMIUM_SESSION_SCRIPT=$PWD/test/drag.session \
    timeout 90 ./builddir/src/chromium-demo
exit=134
demo: uri=... title=DND-PAGE loading=0 back=0 forward=0
nested: pointer becomes grabbing
listener function for opcode 2 of wl_data_source is NULL
Gdk-Message: Lost connection to Wayland compositor.
```

和 `/tmp/host4.log` 那三行一模一样。同一个命令在**修复后**：

```
exit=0
nested: pointer becomes grabbing
nested: a source supports actions 0x3, which is nothing to do here
nested: ignoring a drag: a surface inside a widget has nowhere to drop
```

顺序也解释清楚了老日志里"为什么没有 `ignoring a drag`"：ozone 先 `create_data_source` →
`offer` → **`set_actions`** → `start_drag`，请求在第二步就把 compositor 打死了，永远走不到
我们那句"这里没有可放的地方"。

而且它不只在拖拽里出现。`/tmp/manyclick.script`（只是反复点击一段已选中的文字）5 次里有 2 次
也发了 `set_actions`，每次都是三行连着出现：

```
nested: pointer becomes grabbing
nested: a source supports actions 0x3, which is nothing to do here
nested: ignoring a drag: a surface inside a widget has nowhere to drop
```

点选中的文字 → 起手的按下+微动就够 Chromium 判定成拖拽。这就是它"偶发但反复回来"的原因。

## 5. 修法：三个槽填上，语义就是"这儿没东西可放"

`src/nested-data-device.c`：

| 位置 | 槽 | 干什么 |
| --- | --- | --- |
| `:707` `data_source_set_actions()` | `data_source_impl.set_actions`（`:720`） | 记下这个 source 支持哪些 action，然后什么都不做 |
| `:620` `data_offer_finish()` | `data_offer_impl.finish`（`:648`） | 客户端说拖完了，我们这边从来没开始过 |
| `:626` `data_offer_set_actions()` | `data_offer_impl.set_actions`（`:649`） | 目标端声明想要哪个 action，没有可放的表面 |

```c
static void
data_source_set_actions (struct wl_client *client, struct wl_resource *resource,
                         uint32_t dnd_actions)
{
  /* Chromium sends this for plain clipboard sources. There are no drags to
   * act on, but the slot has to be filled: opcode 2 of wl_data_source with an
   * empty implementation kills the compositor, the host and every client. */
  g_debug ("nested: a source supports actions 0x%x, which is nothing to do here",
           dnd_actions);
}
```

不拒绝、不断开、不发 error：拒绝一个 v3 请求会让客户端以为协议不兼容，而我们只是没有拖放目标。

## 6. 这类 bug 全仓扫一遍

`test/audit-impl-slots.py`：从装好的 `wayland-server-protocol.h` 加上 `builddir/*-server-protocol.h`
解析出每个 `struct X_interface` 的成员顺序（这就是 opcode 顺序），再解析 `src/*.c` 里每个
`static const struct X_interface = { ... }` 初始化式，把没填的槽连它 `SINCE_VERSION` 一起报出来。
有漏就直接非 0 退出，所以能当检查用。

```
$ python3 test/audit-impl-slots.py
0 uncovered slot(s), 0 unknown interface(s), 29 implementation struct(s)
```

修复前它报的是 **3 个空槽**——正好是这三个。任何一次版本升级之后都要重跑它。

静态审计只看初始化式，运行时会来的另一半现在是 `test/shm-client.c` 的 `--dnd-requests`：
第一个连接用 `application/x-chromiumgtk-dnd-probe` 这个 mime 拿下选择权并发
`wl_data_source.set_actions`，**第二个连接**（同一个进程的另一条 Wayland 连接，对 compositor
来说就是别的客户端）收到这个 offer，再发 `wl_data_offer.set_actions` + `wl_data_offer.finish`。
compositor 不会把客户端自己的选择权还给它自己（`data_device_advertise()` 里那句
"nobody is handed back their own copy"），所以要两个连接才凑得出"有主 + 有人收"。

```
$ ./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --dnd-requests
client: sent wl_data_source.set_actions and took the selection
client: the second connection was offered application/x-chromiumgtk-dnd-probe
client: sent wl_data_offer.set_actions and .finish on id 4278190080
client: still connected on both halves (main error=0 second error=0)
selftest: finished at step 27, 139 fds open (119 at start)
```

挑这个 mime 是有原因的：`pick_text_mime()` 在没有文本 mime 时会退回第一个 mime，于是 compositor
还是来 `send` 了一趟，但 `clipboard_transfer_finish()` 里 `mime_is_text()` 为假就不往外面写——
所以这条测试**不碰宿主剪贴板**（日志里那句 `which is not bridged` 就是这件事）。

## 7. 验证

| 跑法 | 结果 |
| --- | --- |
| `test/drag.session` × 3（修复后） | exit=0，`supports actions` 3/3、`ignoring a drag` 3/3、`is NULL` 0 |
| `test/drag.session`（修复前，同一命令） | exit=134，`opcode 2 of wl_data_source is NULL` |
| dndprobe `source` / `offer` / `both` | probe error=0，compositor 活到 timeout 才被收（exit 124，不是崩溃）；`source` 打出 `supports actions`，`offer` 打出 `wants action` + `finished a drag` |
| `/tmp/manyclick.script` × 5 | 4–8 次 copy，0 次 abort，其中 2 次带 `set_actions` |
| `seat-selftest` 六模式 | 全部 step 24；fd 115 → 133（`--refused` 128） |
| `test/audit-impl-slots.py` | 0 空槽 |
| `seat-selftest --dnd-requests`（修复后） | exit=0，三条 nested 日志都出现：`supports actions 0x3`、`wants action 0x1 of 0x1`、`finished a drag that was never started` |
| 同上，把三个槽再掏空（对照） | exit=134，`listener function for opcode 2 of wl_data_source is NULL`——仓库里第一次自己复现出这个死法 |

## 8. 还没做的

- **拖放本身还是没有**：`start_drag` 继续拒绝，`data_offer_finish` / `set_actions` 只是把槽占住。
  要真做拖放得先有"往 widget 外面放"的目标，那是另一件事。
- ~~dndprobe 还在 `/tmp`~~：已经落成 `test/shm-client.c` 的 `--dnd-requests`（第 6 节），`/tmp` 那份
  可以扔了。seat-selftest 不用改——它本来就把 `argv[3:]` 转交给客户端。
- **`--dnd-requests` 不在默认闸门里**：六模式那条命令不带它。要让它每次自测都跑，就得把它加进
  #26 的 `meson test`，而不是让每个跑的人记一条命令行。
- **seat-selftest 没有拖拽回归**：现在只有 demo 脚本这条路径会碰到 `set_actions`。
- **#28 升版本前必须先跑审计**：`xdg_toplevel_impl` 连 `.close` 都没填，这类空槽在别的接口上一旦
  版本抬起来就是同一个死法。
