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
    CHROMIUM_HELPER=$PWD/helper/build/Release/cefnest-helper \
    CHROMIUM_URL=file://$PWD/test/drag-page.html \
    CHROMIUM_SESSION_SCRIPT=$PWD/test/drag.session \
    timeout 90 ./builddir/src/cefnest-demo
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
第一个连接用 `application/x-cefnest-dnd-probe` 这个 mime 拿下选择权并发
`wl_data_source.set_actions`，**第二个连接**（同一个进程的另一条 Wayland 连接，对 compositor
来说就是别的客户端）收到这个 offer，再发 `wl_data_offer.set_actions` + `wl_data_offer.finish`。
compositor 不会把客户端自己的选择权还给它自己（`data_device_advertise()` 里那句
"nobody is handed back their own copy"），所以要两个连接才凑得出"有主 + 有人收"。

```
$ ./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --dnd-requests
client: sent wl_data_source.set_actions and took the selection
client: the second connection was offered application/x-cefnest-dnd-probe
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
| `meson test -C builddir`（含 #24 之后，现共 9 条） | 9/9 OK；上面这些跑法从此不用手敲，`--suite selftest` / `--suite protocol` 各跑一半 |

注：表里 `step 24`、`fd 115 → 133` 是 #30 当时那轮的数，脚本后来在 #24 里长了 3 步。#32 给脚本末尾
加了 `--gtk-client` 那一步，#31 又把滚轮的"手势结束"单列成一步（`axis-stop`），所以现在是
`finished at step 29`。fd 的**净增**才是有用的数，基线绝对值每轮都会飘；`meson test -v` 这一轮按模式
量出来是 plain `11 → 28`（净增 17）、`--refused` `11 → 23`（12）、`--dmabuf` `11 → 42`（31）、
`--dnd-requests` `11 → 30`（19）、`--gtk-client` `11 → 28`（17，和 plain 一格不差，说明那个真 toolkit
客户端走掉之后连接没留下 fd）。跟手敲那轮的 18/13 差一个 fd，这一格差没去追——要看的是同一模式换
跑法、换几轮，净增不往上走。

`#31` 还改了闸门的另一件事：`shm-client` 现在会**检查事件形状**（一条 `axis_stop` 不许和它要结束的
`axis` 同一帧），不合格就 `exit (1)`，`seat-selftest` 等子进程的报告、非零状态或信号立刻让 run 停在
那一步并返回 1（`test/seat-selftest.c:136`）。以前客户端半路死了没人管——空事件槽那种死法（本篇
第 1 节）在 selftest 里本来是看不见的。

## 8. 还没做的

- **拖放本身还是没有**：`start_drag` 继续拒绝，`data_offer_finish` / `set_actions` 只是把槽占住。
  要真做拖放得先有"往 widget 外面放"的目标，那是另一件事。
- ~~dndprobe 还在 `/tmp`~~：已经落成 `test/shm-client.c` 的 `--dnd-requests`（第 6 节），`/tmp` 那份
  可以扔了。seat-selftest 不用改——它本来就把 `argv[3:]` 转交给客户端。
- **`--dnd-requests` 已经在默认闸门里**：`meson test -C builddir` 现在跑 9 条——槽位审计 +
  八个 seat-selftest 模式，`--dnd-requests` 是其中一个（`meson test --suite selftest`）。没有
  显示设备时 seat-selftest 自己报 skipping 并退 0，所以这套闸门在没人坐的机器上也能跑。
- **seat-selftest 没有拖拽回归**：现在只有 demo 脚本这条路径会碰到 `set_actions`。
- **#28 升版本前必须先跑审计**：`xdg_toplevel_impl` 连 `.close` 都没填，这类空槽在别的接口上一旦
  版本抬起来就是同一个死法。

## 9. 第二个死法：wl_shm 的槽 1（本篇第 8 节最后那条预言应验了）

日期：2026-10-23。`meson test` 在 Debian 13（libwayland 1.23.1）上报红：

```
MISSING  src/nested.c shm_impl  slot 1 = release  since 2
1 uncovered slot(s), 0 unknown interface(s), 32 implementation struct(s)
```

`shm_impl` 从来只填了 `create_pool`。槽 1 在 1.22 的头文件里叫 `destroy`，1.23 把它
改名成 `release` 并标成 `since=2`——**opcode 没动**，`/usr/share/wayland/wayland.xml` 里
`wl_shm` 现在是 `version="2"`，成员顺序 `create_pool`(0)、`release`(1)。

### 9.1 谁能真的发出来

- **1.23 上发不出来**：全局报的是版本 1，而 libwayland-server 自己就按 `since` 拦
  （头文件里有两条不同的串，实测走的是后者）：

  ```
  $ strings /usr/lib/x86_64-linux-gnu/libwayland-server.so.0 | grep -a 'invalid method'
  invalid method %d, object %s#%u
  invalid method %d (since %d < %d), object %s#%u
  ```

  所以在 1.23 上这条请求连实现表都碰不到，闸门是**红但打不死人**的。
- **1.22 上是日常路径**：那边 `destroy` 的 `since=1`，而 `wl_shm_destroy()` 就是把这个
  opcode 写上线的包装函数。Ubuntu 24.04——本项目 deb/rpm 的构建与安装目标——正是 1.22。
  也就是说：**在那台机器上，一个客户端只是正常关掉自己的 shm 代理，就能把嵌套合成器
  连同宿主进程 `wl_abort`**。这才是这条真正要修的理由。

### 9.2 修法：填槽，并把版本升到能测它

`src/nested.c` 里槽 1 用**位置式**初始化，不是疏忽：`destroy` 这个名字在 1.23 不存在，
`release` 在 1.22 不存在，只有按下标填才同时对得上两边。`shm_release()` 就一句
`wl_resource_destroy (resource)`——协议写明"经本接口创建的对象不受影响"，池子各有自己的
引用，确实只有这个资源要走。

全局版本从 1 抬到 2。不抬的话，客户端绑到版本 1 的代理发 `release` 会被 libwayland-server
按 `since` 拦掉，运行时闸门就只能测"版本检查"而测不到我们的槽。抬到 2 之后这条请求才是
合法的，闸门才有东西可判。

### 9.3 顺手量出来的两件事

**(a) 帧头的字节序，本篇第 6 节那个探针一直是错的。** `--wrong-object` 注释里写着
"a wayland frame is size, sender id, opcode"，真实布局是**对象 id、然后一个
`(size<<16)|opcode` 的字**。拿 strace 量的（同一份 libwayland 自己写出去的帧）：

```
wl_display.bind   (opcode 1, 12 字节) → 01 00 00 00 | 01 00 0c 00
wl_buffer.destroy (opcode 0,  8 字节) → 05 00 00 00 | 00 00 08 00
wl_shm_pool.destroy(opcode 1, 8 字节) → 03 00 00 00 | 00 00 08 00
```

按旧写法发出去的帧，compositor 回的是 `invalid method 16`——那是把 size 字段当成 opcode
读了。**这个闸门此前一直在判一条畸形帧，而不是它宣称在判的那个错对象。** 改对之后同一条
请求的回答变成：

```
invalid object (8), type (zwp_primary_selection_source_v1), message set_selection(?ou)
invalid object (8), type (wl_surface), message set_selection(?ou)
```

正是 libwayland-server 的参数类型检查，也是这个闸门本来要量的东西。

**(b) 审计自己漏了 NULL 的写法。** 把槽改回 `{ shm_create_pool, NULL }` 之后
`audit-impl-slots.py` 仍然报 0 空槽：它的位置式正则 `^\s*(?!/\*|\.)(\w+),` 把 `NULL`
也当成"填过了"。设计式同理（`.destroy = NULL` 也算 provided）。libwayland-server 按下标
取函数指针，不关心源码怎么拼这个洞，所以两种写法都得算洞。修完之后同一份变异报
`MISSING ... slot 1 = release since 2`。

### 9.4 运行时闸门：`--shm-release`

`test/shm-client.c` 新增模式：把主连接的 `wl_shm` 绑到版本 2，手写那 8 字节裸帧发
opcode 1，然后要求这条连接继续被服务。`test/meson.build` 的模式表里加了一个
`shm-release`，`meson test` 从 27 条变 28 条。

```
$ ./test/ci-session.sh ./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --shm-release
client: sent opcode 1 of wl_shm (destroy on the 1.22 headers, release on the 1.23 ones)
        on id 4 and the connection is still live
client: shm-release gate passed: opcode 1 of wl_shm was taken, and 38 frames later this
        connection is still being served
selftest: finished at step 52, 162 fds open (129 at start)
RESULT shm-release: exit 0
```

| 跑法 | 结果 |
| --- | --- |
| 填槽（`shm_release`） | exit 0，请求被接走，之后 38 帧仍在服务 |
| 同一份代码把槽改成 `NULL` | **exit 134**，日志里就是本篇第 1 节那个死法：`listener function for opcode 1 of wl_shm is NULL` |
| `audit-impl-slots.py`，填槽 | 0 uncovered |
| `audit-impl-slots.py`，槽写 `NULL` | MISSING slot 1 = release since 2（修审计之前这一格是 0 uncovered，即漏判） |
| `--wrong-object` 帧序修正后 | exit 0，但拒绝理由是 `invalid object (8), type (...)` 而不是 `invalid method` |
| `meson test -C builddir`（嵌套 mutter 里） | **28/28 OK**，含 `impl-slot-audit` 与新的 `seat-selftest-shm-release` |

注：裸帧只能在 `wl_display_flush()` 之后直接写 socket；写完这个代理就是一个死 id，所以
`--shm-release` 之后本模式不再碰 `wl_shm`，别的模式也不发这条请求（要再建池子的模式会拿
死 id 去建）。

### 9.5 重量过 bind 表，以及还没做的

- **版本 2 抬上去之后 `docs/internals/protocol-ceilings.md` §2 已重量**（2026-10-23，
  在 `./test/ci-session.sh` 的 headless 嵌套 mutter 里跑 A 与跑 B）。两个结论：
  真实客户端**要**的版本没变——GDK 和 Chromium 的两条连接都仍然 `bind(2, "wl_shm", 1)`，
  所以抬广告版本不会让它们开始发 `release`，槽 1 的意义仍然在 1.22 那条 `destroy` 路径上；
  顺带量出旧表记错了两处（漏了 `wl_output` 与 `zwp_primary_selection_device_manager_v1`，
  多记了一个本轮根本不绑的 `zwp_text_input_manager_v3`），以及 §1 那条"mutter 独有协议
  bind 行数为 0"的归属判据不成立——selftest 本体为了画 GdkTexture 自己就连着外层
  mutter。这些都在那篇里改了。
- **1.22 那条日常路径本机量不到**：这台机器只有 1.23。`wl_shm_destroy()` 在 1.22 会发
  opcode 1 这件事，依据是 `strings` 里 libwayland-server 的两条版本检查串、1.23 头文件里
  `WL_SHM_RELEASE=1 / since 2` 的存在，以及 `wl_shm_interface` 的成员顺序；没有在真的
  1.22 上跑过一次"客户端正常关闭 → 修复前 abort"。
