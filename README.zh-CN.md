# CEFNest

[English](README.md) | 简体中文

一个 GTK 4 widget，显示由**真正的 Chromium** 渲染的网页——通过一个运行在同一进程内的
Wayland compositor 合成到 widget 里。

两条硬性约束决定了一切：

- **原生 Wayland。** 不用 XWayland，不做 X11 client，没有 `XEMBED`。页面由 Chromium 自己的
  `ozone/wayland` 后端绘制。
- **不用 OSR。** 没有"离屏渲染"再拷进 Cairo surface 这回事。Chromium 创建一个 Wayland
  surface，我们的 compositor 像任何 compositor 一样接收它，并以 `GdkTexture` 的形式画进
  widget。

这就排除了所有不自带 compositor 的"把浏览器嵌进 GTK"的做法，所以这个仓库自带一个。

## 工作原理

```
 GTK 4 应用
   └── ChromiumView（一个 GtkWidget）
         └── NestedCompositor            src/nested*.c — 一个 libwayland-server compositor
               · 在 $XDG_RUNTIME_DIR 里开自己的 socket
               · 接收 shm / dmabuf buffer，合成 surface 栈
               · 把 widget 真实的指针、键盘与输入法转发给 client
         ▲
         │ Wayland 协议（socket 的 client 一侧）
         │
 cefnest-helper                          helper/ — 一个 CEF 应用，独立进程
   · 以 WAYLAND_DISPLAY=<嵌套 compositor 的 socket> 启动
   · Chromium 把页面渲染进那个 compositor
   · 控制面：unix socket 上的 NDJSON，双向
```

三个部件，各干一件事：

| 部件 | 文件 | 职责 |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | compositor：globals、surface、damage、输入、剪贴板、跨窗口边界的双向拖放 |
| `ChromiumView` | `src/chromium-view.c` | widget：把 GdkSurface 的输入交给 compositor，绘制合成结果 |
| `ChromiumSession` | `src/chromium-session.c` | 控制面：启动/停止 helper、跟踪页面状态、执行 JavaScript、代理下载、查找、页面右键菜单与权限请求 |

compositor 通告的 globals 及版本见 `src/nested-internal.h`：`wl_compositor`、`wl_shm`、
`wl_seat`（8，这样 pointer 和 keyboard 能携带 `axis_value120`）、`wl_output`、
`wl_data_device_manager`（3）、`xdg_wm_base`、`zwp_linux_dmabuf_v1`（5，带 feedback）、
`wp_viewporter`、`wp_fractional_scale_manager_v1`、`zwp_text_input_manager_v3`、
`wp_cursor_shape_manager_v1`。

## 依赖

在开发机上实测（Debian 13、`linux 6.12`）：

| 软件包 | 实测版本 | 最低 |
| --- | --- | --- |
| gtk4 | 4.18.6 | 4.14 |
| wayland（server + client） | 1.23.1 | 任何带 `axis_value120` 的版本（1.21+） |
| wayland-protocols | 1.44 | 须带 `staging/cursor-shape`、`staging/fractional-scale`、`unstable/text-input` |
| xkbcommon | 1.7.0 | — |
| gbm / libdrm | 25.0.7 / 2.4.124 | — |
| json-glib | 1.10.6 | — |
| meson / ninja / cmake | 1.7 / 1.12 / 3.16+ | meson 1.3（demo 的 `$ORIGIN` rpath 用 `fs.relative_to` 构建） |
| CEF（`cef_binary_*_linux64_minimal` / `linuxarm64_minimal`） | 154.0.34+g14c5a08+chromium-154.0.8037.98 | 任何能用它自己的 CMake helpers 构建的版本；arm64 的 URL 路径已接好，但尚未在 arm64 主机上测过 |

`wayland-scanner` 必须在 `PATH` 上（协议代码在构建期生成——仓库里没有 checked-in 的副本）。

## 构建

两个阶段，第一阶段可选。

```bash
# 1. 拉取 pinned 的 CEF 二进制发行包（gitignore，解压后约 500 MB）
./scripts/fetch-cef.sh

# 2. Chromium 侧 —— 自动发现 third_party/cef/dist
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. widget、demo、测试
meson setup builddir
ninja -C builddir
```

`scripts/fetch-cef.sh` 锁定精确的 CEF 构建；换发行包用 `-DCEF_ROOT=…` 传入。升级 pinned
版本意味着重跑整套门禁——一个 Chromium 里程碑同时会改变 client 请求哪些 Wayland 协议
（`docs/internals/protocol-ceilings.md` 是重测记录）。

跳过第一阶段一切照常构建：demo 会改驱动 `test/shm-client` 而不是 Chromium，这已足够
覆盖 compositor、seat、剪贴板桥和弹窗。

## 安装

组件以共享库安装，带 pkg-config 文件、头文件和 demo；`docs/guides/packaging.md` 记录了每一样
装到哪里，以及一个 session 如何决定启动哪个 helper 二进制。

```bash
meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install
```

| 选项 | 默认 | 决定什么 |
| --- | --- | --- |
| `helper-path` | 空，由 `prefix` + `libexecdir` + 主机架构推导 | 编进库里的路径，没人指名时用它 |
| `helper-build-dir` | `helper/build/Release` | cmake 留下的东西：开发期回退，也是被安装的那个 |
| `install-helper` | `false` | 同时安装 helper 和整套 CEF runtime——1.5 GB，所以是可选项 |
| `install-demo` | `true` | 安装 `cefnest-demo`，以及没有 Chromium 时它回退到的 `shm-client` |

在一棵 cmake 没跑过的树上开 `install-helper` 是 configure 期报错，而不是一个空目录配上
一行信心满满的 "Installing subdir"。安装出去的 helper 对构建它的树一无所需——CEF runtime
跟着它走、从它身边解析，这是把整个 `third_party/cef/dist` 目录挪走之后实测的
（`docs/guides/packaging.md` §9）。

## 运行

```bash
# 真家伙 —— cmake 构建过 helper 后不需要任何环境变量，
# 因为库自己找得到（docs/guides/packaging.md §2）
CHROMIUM_URL=https://example.com ./builddir/src/cefnest-demo

# 不掺 Chromium：指一个不存在的 helper，demo 会回退到用嵌套 compositor
# 驱动 test/shm-client
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo

# 装好的那份，从 prefix 起动，环境里什么都不用
CHROMIUM_URL=https://example.com $HOME/.local/cefnest-stage/bin/cefnest-demo
```

`CHROMIUM_HELPER` 仍然是"设了任意值就强制 session 模式"，`test/fake-helper.py` 就是这么
在哪儿都没有浏览器的情况下被脚本驱动的。

demo 的全部配置来自环境变量，因为它的意义就是可复现的运行：

| 变量 | 作用 |
| --- | --- |
| `CHROMIUM_HELPER` | 设了（任意值）即强制 session 模式；这里的路径必须含 `/`，且按原样使用——哪怕不存在 |
| `CEFNEST_HELPER` | 同一条规则提前一步：含 `/` 的路径压过 `CHROMIUM_HELPER`。两者都没设时，库自己解析 helper——先编进库的安装路径，再是构建时旁边的 cmake 树（`docs/guides/packaging.md`） |
| `CHROMIUM_URL` | 要打开的页面 |
| `CHROMIUM_SESSION_SCRIPT` | 一行一条命令的脚本文件：`load`、`back`、`forward`、`reload`、`reload-hard`、`stop`、`zoom-in`、`zoom-out`、`zoom-reset`、`js`、`js-expr`、`a11y-on`、`a11y-off`、`frames`、`wait`、`shot`、`dump`、`hover`、`click`、`drag`、`drag-hold`、`release`、`drop-in <x0> <y0> <x1> <y1> [text] [mimes]`、`drop-in-hold`、`drop-in-move <x> <y>`、`drop-in-drop`、`drop-in-cancel`、`surfaces`、`wheel`、`download-accept <path>`、`download-reject`、`download-accept-name <substring> <path>`、`download-reject-name <substring>`、`new-tab <url>`、`new-window <url>`、`select-browser <id>`、`close-browser <id>`、`find <pattern>`、`find-case <pattern>`、`find-next`、`find-prev`、`find-stop`、`menu-pick <id>`、`menu-dismiss`、`perm-accept <id>`、`perm-deny <id>`、`perm-dismiss <id>`、`close`、`quit` ——见 `test/*.session`。导航、JS、缩放、静音、下载、查找、菜单与权限步骤需要 helper session——没有时这些步骤以状态 1 结束运行而不是被跳过；而 `wait`、`shot`、`dump`、`surfaces`、`hover`、`click`、`drag`、`wheel`、`drop-in*` 和 `close` 只驱动嵌套 compositor |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | N ms 后抓取 widget 写一张 PNG 然后退出 |
| `CHROMIUM_WINDOW` | `fullscreen` 或 `maximize`，用于确定的几何 |
| `CHROMIUM_CLIPSET` / `CHROMIUM_CLIPWATCH` | 预置宿主剪贴板，并把它桥进页面 |
| `CHROMIUM_PRIMARYSET` / `CHROMIUM_PRIMARYWATCH` | 预置/读取宿主主选区——中键那个剪贴板 |
| `CHROMIUM_SPELLCHECK` | 不设或 0 关拼写检查（默认）：helper 传 `--disable-spell-checking` 并对每个可编辑节点置 `spellcheck=false`。`1` 恢复 Chromium 的拼写建议及其菜单的折叠光标 |
| `CHROMIUM_A11Y` | `1` 在 helper 启动时强制开无障碍。不设时 session 监听 session bus 上的 `org.a11y.Status.ScreenReaderEnabled`，读屏器出现/消失时发运行时 `a11y` 命令 |
| `CHROMIUM_FRAME_METRICS` | 不设或 0 安静；`1` 每秒打一行 `demo: frame-count N`。同一计数器是 `chromium_session_get_frame_count()` / 脚本步骤 `frames` |
| `CHROMIUM_HELPER_NO_SANDBOX` | 恰好为 `1` 才不带 Chromium 沙箱跑 helper；其它任何值——包括手滑打错——都保持开启（`docs/internals/sandbox-policy.md`） |
| `CEFNEST_DOWNLOAD_DIR` | demo 面板自动接受下载的位置；默认 XDG 下载目录（`docs/features/download-path.md`） |
| `CEFNEST_DOWNLOAD_ASK` | 任意值：每个下载都等 保存/丢弃 按钮或脚本步骤，而不是直接落盘 |
| `CEFNEST_DOWNLOAD_NO_SAVE_AS` | 任意值：图片下载跳过 `GtkFileDialog`，走普通的下载目录/询问路径 |
| `CEFNEST_PERMISSION` | `ask`（默认：每个请求一行 允许/拒绝）、`deny` / `allow`（到达即回答，行留着当记录）、或 `ignore`（根本不接问题——session 于是拒绝它，这正是没有权限 UI 的宿主会得到的）（`docs/features/permission-prompt.md`） |
| `CEFNEST_DRAG_OUT` | demo 怎么处理离开窗口的页面拖动。默认：取走 payload、用窗口自己的 `GdkDrag` 起拖；图片字节落成临时文件以 `GdkFileList` 提供，文本就是文本。`take`：读出并记录但不自己起拖——自动化运行用这个，因为脚本化的按键给不了 GDK 起拖需要的 serial。`off`：根本不连 `::drag-out`，页面保留自己的拖动直到松键——没有 handler 的宿主得到的就是这个（`docs/features/drag-loop.md`） |
| `G_MESSAGES_DEBUG` | **空格分隔**的域：`all`，或 `nested chromium-view chromium-session`——逗号列表什么都匹配不到 |

## 最小宿主

`examples/minimal.c` 是三个对象的版本——窗口、`ChromiumView`、`ChromiumSession`——
不带任何 demo 的外壳。随库一起构建后运行：

```bash
ninja -C builddir
CEFNEST_URL=https://example.com ./builddir/examples/cefnest-minimal
```

必需的顺序、生命周期，以及导航/JS/下载/菜单/主选区/无障碍的 API 对照表在
`docs/guides/minimal-host.md`。

`examples/host.c` 是同样三个对象再往上一步：页面站在答案上的那四个信号。它为
`::download-begin` 指定路径（于是页面文件落到哪里整个由它说了算），用 允许/拒绝 行回答
`::permission-request`，把 `::script-result` 读进一个日志面板，并以 `::browser-created`
为键生成标签行。`::context-menu` 在那里**故意不连**，好让"什么都不写也自带的菜单"和宿主
自己的菜单并排可见。它接受脚本文件驱动，所以不需要键盘就能测：

```bash
printf 'load http://gate.test/download?name=report.pdf&bytes=4096\nwait 800\nsave /tmp/dl\nwait 1600\nquit\n' > /tmp/steps
CEFNEST_HOST_SCRIPT=/tmp/steps CEFNEST_HELPER=$PWD/test/fake-helper.py \
  ./test/ci-session.sh ./builddir/examples/cefnest-host http://gate.test/start
```

动词：`load tab select close js save drop allow deny wait quit`。这些运行测到的东西——
包括一个根本关不掉的标签——写在 `docs/guides/minimal-host.md` §5。

两个示例读它们自己的两个环境变量（上面 demo 的表不管它们——它们属于示例，不属于 demo）：

| 变量 | 作用 |
| --- | --- |
| `CEFNEST_URL` | `chromium_session_start()` 打开的页面；不设或为空，两个示例都回退到 `https://example.com`（`examples/minimal.c:34`、`examples/host.c:1039`） |
| `CEFNEST_HOST_SCRIPT` | （仅 `examples/host.c`）一行一个动词的文件，从 `::ready` 驱动，示例因此可以无键盘测试 |

## 测试

```bash
meson test -C builddir                       # 25 个测试
meson test -C builddir --suite selftest      # 二十个 seat selftest + map-cycle
meson test -C builddir --suite protocol      # 三个源码审计
meson test -C builddir --suite unit          # helper 路径解析规则
```

`test/ci-all.sh` 是无人值守的整套电池：下面每一个嵌套 session 能承载的门禁，一门一个
嵌套 compositor，末尾一张判定表——`event-shape-gate.sh` 是它不承载的唯一一个控制通道
门禁。`test/` 里其余的脚本是写进文档的手跑门禁，所以没有一个是 `meson test`：它们在谁的
桌面上就开谁的真窗口——`download-gate.sh`、`find-gate.sh`、`dnd-gate.sh`、`menu-gate.sh`、
`permission-gate.sh`、`primary-gate.sh`、`pixel-gate.sh`、`multi-browser-gate.sh`、
`sandbox-soak.sh`、`event-shape-gate.sh`、`framing-gate.sh`、可选的真实页面批次
`real-sites.sh`，以及该批次逐站调用的单站 runner `real-site-run.sh`。
`test/ci-session.sh` 是任何一个都跑在不碰任何人桌面的方式。

`seat-selftest` 启动嵌套 compositor，对它拉起 `shm-client`，跑脚本化的指针/键盘/输入法
序列并打印 client 看到的东西。每个模式请求一组不同的协议对象：

```bash
./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --popup
```

模式有 `--dmabuf`、`--text-input`、`--cursor-shape`、`--popup`、`--refused`、
`--pool-budget`、`--wrong-object`、`--pool-shrink`、`--empty-copy`、`--popup-depth`、
`--damage-scale`、`--drag-fake-serial`、`--drag-no-source`、`--dnd-requests`、`--drag-out`、
`--drag-in`、`--fullscreen`、`--second-window`、`--gtk-client`。没有显示器的机器上每一个
都退出 77，也就是 meson 读作 SKIPPED 的值——skip 和 pass 同页是一种把"未测的规则"藏进
绿灯的方式。

`map-cycle` 是另一个依赖显示器的运行：它把 widget 藏起再显示，数 GLib 手里留下的信号
槽位，所以一个在每次 `map` 挂上、没有拆下的 handler 过不了它。同一条规则的静态一半
`map-arms-audit` 读源码，抓程序数不到的东西——`GtkEventController` 没有公开列表。
`docs/internals/map-cycle.md` 是两者的记录，含各自被验证过的变异。

`--pool-budget` 是 client 当攻击者那个：它交出十六个 100 MB 的 pool——每个单独看都合法
——并且全部保活，除非 compositor 收下一些再拒绝下一个，否则运行失败。对 1 GiB 单连接
上限实测收 10 拒 1（`docs/internals/resource-limits.md` §2）。

`--dmabuf` 在 compositor 的导入侧打分：任何拒绝都会让 client 回退到 `wl_shm` 且照样退出
0，所以这次运行数的是 compositor 自己日志里真正导入了什么，buffer 被拒或 dmabuf 已通告
却一个都没到就失败。没有可用 render node 的机器会自己说明——那次运行是回退证据，不是
导入证据。

`--gtk-client` 不是 `shm-client` 的另一个口味：它以自己的 compositor 的 GTK client 身份
再拉起一份 `seat-selftest` 来收尾，那个 client 被自己的断言杀掉就失败。裸 client 在事件
到达时只是设个标志，所以合法的事件序列压不垮它——toolkit 可以，而且垮过
（`docs/internals/clipboard-offer-during-client-init.md`）。

裸 client 自己也查一件事，运行会在上面停下：一个 `wl_pointer.axis_stop` 不得与它所结束
的 `axis` 事件同处一个 `wl_pointer.frame`——那正是 Chromium 把滚动增量整个丢掉的形状
（`docs/features/wheel-resolution-and-key-repeat.md`）。`seat-selftest` 等它的子进程而不是轮询它，
所以 client 死于信号或非零状态时，测试在那一步失败，而不是把剩下的脚本念给没人听。

`--drag-in` 是把这条压得最狠的模式：目的地是裸 client，所以它是唯一能判断"交到手上的
字节是不是承诺的字节"的地方，它自己比对，不一致就退出 1，而不是把判定留给一行日志。

`--second-window` 是多窗口输入门禁，也是唯一一次 client 的画面**本身就是**断言。一个
`shm-client` 在自己的第一个 toplevel（`nested-shm-test-second`）旁再映射一个，把
`wl_keyboard` 点名的窗口涂红（`#c04040`）、另一个涂绿（`#40c040`），每次 enter/leave
都重画——一个 client 的两个 toplevel，因为两个 CEF 窗口也是一个 helper 的两个
toplevel。然后 `seat-selftest` 在每个窗口点名输入 surface，从**不**拥有输入的窗口和拥有
输入的窗口各调用一次 `nested_compositor_clear_input_surface()`，并在每个检查点核对三件
可能不一致的事：compositor 自己的 `keyboard_focus`/`pointer_focus`、
`nested_compositor_get_toplevels()` 清单，以及用 `gdk_texture_download()` 抠出的每个
toplevel 的中心像素。去掉 owner 守卫、去掉 `clear_input_surface` 里的 retarget，各自都让
一次绿跑变红——这就是像素必须进断言的原因：一个只查状态、永远不看线的门禁不是门禁
（`docs/internals/empty-request-slot.md`）。步骤表和这次运行还说不了的话在
`docs/features/multi-browser.md`。

compositor 发什么只是一半，所以滚轮也在真正要紧的地方被检查：

```bash
GDK_BACKEND=wayland CHROMIUM_HELPER=$PWD/helper/build/Release/cefnest-helper \
  CHROMIUM_URL=file://$PWD/test/wheel-page.html \
  CHROMIUM_SESSION_SCRIPT=$PWD/test/wheel.session ./builddir/src/cefnest-demo
```

`test/wheel.session` 在每个 `wheel` 步骤后读 `document.scrollingElement.scrollTop`，
期望每格 120 CSS px——`wheel 0 3` 是 360，紧随其后的半格是 420。

`test/audit-impl-slots.py` 是同一想法的静态一半：它读生成的协议头和 `src/` 里每个
`wl_*_interface` 初始化器，client 可达的请求槽位有 NULL 就失败——空槽位不是拒绝，它把
compositor 整个 abort 掉（`docs/internals/empty-request-slot.md`）。

`test/uinput-inject.c` 通过 `/dev/uinput` 重放 evdev 事件来驱动 widget 本身，需要对节点
的写权限（`sudo`）。

`test/download-gate.sh` 是下载这一半，它保持手跑门禁身份因为开真窗口：对
`test/fake-helper.py` 十次、再对真 Chromium 十次，覆盖整个矩阵的 41 条断言——文件落地、
名字去重、建议名里的 `../` 出不了目标目录、拒绝留下空目录、飞行中取消、以及一个永远
不回答的宿主。真 Chromium 的取消跑在一个每 100 ms 滴 32 KiB、总 2 MiB 的本地服务器上：
一次写完的话，localhost 下载在脚本来得及拒绝之前就结束了，断言会靠竞态而不是取消路径
通过。`docs/features/download-path.md` §6 有整张矩阵。

`test/find-gate.sh` 是查找这一半，同一手跑门禁约定。六次运行——四次对
`test/fake-helper.py`，两次对 `test/find-page.html` 上的真 Chromium，34 条断言——fake
那几次的意义在于它们重放的是**实测的**回复形状而不是头文件里的措辞：一个请求以一到多
条计数更新加一条 final 回答、identifier 按请求而非按搜索移动、无匹配的搜索只答一次。
两次真运行对同一页面数折叠（8）和区分大小写（7）两种数，断言就是这样证明 `match_case`
到达了 Blink 而不是在宿主侧被过滤；它们读两次 `scrollTop`——步骤前后各一次——因为
"搜索不动页面、步骤把匹配滚进来"这条测量正是 wire 上没有匹配矩形的原因。

`test/dnd-gate.sh` 是拖放这一半，`test/dnd-page.html` 上六次真 Chromium 运行、59 条断言。
它回答整个进程内路线曾经卡住的那个问题：widget 里的浏览器到底认不认
`wl_data_device.enter`，还是靠自己的内部 drag manager 处理窗口内拖动。页面自己的事件
日志说它认——脚本喂给 seat 的坐标就是页面在 `dragenter`、`dragover` 和 `drop` 上报告的
坐标。其余几次运行抓的是被丢在半路的状态机：一个不收 payload 的目的地、第一次之后的
第二次拖动、一次被拖住不放的 drag（好让拖影加入和离开绘制栈的画面被看见，随后一次
点击证明 seat 回来了）、一次完全离开窗口的拖动——compositor 靠从页面自己的 source 上读
走 payload、叫停那个 drag、把它递给宿主的东西打印出来——以及反方向的拖动，compositor
作为 source 引入、由页面来谈。最后那次是四个入站 drag，因为这个方向的全部产品就是必须
交还给宿主的判定：拖到目标上方被收下、拖到什么都不处理的元素上方被退回、宿主叫停时被
退回、之后再被收下。改写判定的那条测量：页面在**被 enter 的一刻**就读宿主 drag 的
payload 两遍，不管之后收不收，所以只有 `wl_data_offer.finish` 能定夺。出站那次运行也是
"没有目的地接受过的 mime，Blink 还会不会应答 `send`"这条测量的落点，它带
`CEFNEST_DRAG_OUT=take` 运行，真指针下不会起宿主拖动。两个
方向的协议半边都不需要浏览器：`meson test -C builddir --suite selftest` 里有
`seat-selftest-drag-out`，`shm-client --drag-out` 既是三个实测类型之一的一次 drag 的
源头也是目的地；还有 `seat-selftest-drag-in`，`shm-client --drag-in` 是一个接受某类型、
按页面被实测的方式读 payload 的活跃目的地，字节对不上就非零退出。门禁把两路输出接进
`tee`，因为 GLib 的 writer 在进程离开 tty 之后把 DEBUG 消息发到 stdout 而不是 stderr，
而 compositor 自己的 trace 是断言的一半。

`test/menu-gate.sh` 是右键菜单这一半，八次运行——六次对 `test/fake-helper.py`，两次对
`test/menu-page.html` 上的真 Chromium——47 条断言。fake 那一半重放决定了整个设计的约束：
Chromium 只握着**一个**未回答的菜单请求，所以五次不回答的右键产生一个事件、页面看到五
次。这就是 session 总是回答的原因——宿主不接信号时给默认菜单、没有 view 或没有命令行可
画时给 dismiss、导航时给 dismiss，因为实测导航**不会**关掉宿主握着的请求。真那一半证明
的是往返而不是画图：`menu-pick 117` 是 CEF 自己的全选，页面读回
`selectionEnd - selectionStart == 18`，所以命令跑在 Blink 里而不是宿主里。断言菜单出现
用的是 session 的 `context menu mapped=1 size=` 行，不是 `shot`——Wayland 上 popover 是
自己的 surface，在 grab 覆盖的 widget 之外。那一行也是没有显示器的显示器满足不了的唯一
断言（它的弹窗被创建、布局、从不映射），所以在 `test/ci-session.sh` 里门禁把那两条报为
skip，其余 45 条照样绿。

`test/image-dnd-page.html` 是手跑的图片拖拽夹具：带一张可拖的 8x8 PNG，宿主把 Chromium
的 `application/octet-stream;name="download.png"` 转成临时文件加 `GdkFileList`，文件管理
器就能复制。它不在任何脚本化门禁里；源侧的 GTK 拖拽需要真实的按下 serial。

`test/spellcheck-page.html` 是干净的拼写检查夹具（一个 textarea，不带链接/图片/选区测试
内容）。拼写检查默认关；设 `CHROMIUM_SPELLCHECK=1` 走建议菜单和它的折叠光标。
`test/primary-page.html` 是中键主选区夹具。`CHROMIUM_PRIMARYSET`/`CHROMIUM_PRIMARYWATCH`
预置和读取宿主主选区；脚本化的左键聚焦 textarea，按键 `274`（`BTN_MIDDLE`）粘贴。宿主→
页面方向用 274 字节实测过；页面→宿主方向需要 Chromium 创建 primary source，这个 CEF 构建
在同一次运行里没有做（`docs/features/clipboard-bridge.md` §5）。

`test/primary-gate.sh` 是主剪贴板的页面选区一半：helper 注入选区监听器，session 把选中
的词写进 Gtk 主剪贴板，门禁用 `CHROMIUM_PRIMARYWATCH=1` 读回。它在无头 ci session 里跑，
现在是 `ci-all` 的默认门禁之一。

`test/real-sites.sh` 是真实页面巡检批次。它对 `test/real-sites.tsv` 里的每个站点
（Bing、MDN、YouTube、Google Maps、Excalidraw）跑一个 `test/ci-session.sh`，然后把
session、transcript、截图、`readback.json` 和一份 `summary.md` 留在
`builddir/real-sites/` 下。它用 `js-expr` 脚本步骤，这样带严格 `script-src` 的页面（MDN）
也能回答：`js` 把代码包进页面的 `eval`，被它们的 CSP 拒绝。它不在默认 `ci-all` 电池里，
因为站点是外部的；`CI_ALL_REAL=1 ./test/ci-all.sh real` 是可选的那行。第一批实测写进
`docs/testing/real-sites.md`。

`test/permission-gate.sh` 是权限这一半，十次运行——七次对 `test/fake-helper.py`，三次对
`test/permission-page.html` 上的真 Chromium——58 条断言。它从不打开摄像头或麦克风：文件里
每个媒体请求都以拒绝收尾，它断言通过的那个 grant 是通知，背后没有设备。fake 重放的是对
浏览器实测的队列形状——宿主没回答的媒体请求压住整个媒体队列，它后面的那个请求只有等
答案回来才会作为自己的请求到达一次；宿主没回答的 prompt 在下一个 prompt 出现的一刻被
当作 `ignore` dismiss。真运行断言页面看到的而不是 trace：`real-deny` 逐字比对十次读回，
`real-answer` 证明宿主的答案到了页面（拒绝的地理定位是 `code1`，接受的通知是
`granted`），`real-park` 用两条顺序断言加四次页面读回钉住媒体队列。每个真运行拿自己的
`XDG_CACHE_HOME`，冷启动也是这么发现的：helper 在没人用过的 profile 上要约三秒才报
`ready`，按固定定时器起的 session 脚本把第一个 `load` 丢在地板上。session 模式现在从
`::ready` 起脚本。

两个门禁从对端那一侧盯控制通道。`test/event-shape-gate.sh` 把 dispatch 比对过的每个事件
名——从 `src/chromium-session.c` 里读出来，不是抄进脚本，找不到 20 个名字就红而不是空转
——全部字段缺席地发一遍，这是"另一个版本的 helper"的形状。它断言零 critical、没有东西
以 `(null)` 到达 demo：把字段守卫拿掉，同一条流让宿主先出 18 条 critical 然后一个
SIGSEGV。`test/framing-gate.sh` 为另一条规则加了 `flood` 段——一条行得有头有尾。一次
运行判三件事：一条合法的 1 MB 事件必须整条读到、一条带 NUL 的行必须整个丢弃而不是半信、
没有换行的字节必须撞上 4 MiB 上限并停掉通道。两个都开真窗口，所以都是与 `menu-gate.sh`
同一约定的手跑门禁；`./test/ci-session.sh ./test/event-shape-gate.sh` 让任何一个都跑在
不碰任何人桌面的方式里。`docs/guides/control-channel.md`"通道读这一侧"是记录，含每条断言被
验证所用的变异。

`test/sandbox-soak.sh` 是沙箱这一半，它断言的是进程树而不是像素：一个 session，四个本地
页面二十次导航、每第七次硬刷新，并在运行内两个时点故意 `kill -9` 每个 renderer。八组
检查——进程树保持体型（9 个进程、8 个带 `--type=`、2 个 renderer，与稳定样本起不变）、
没有谁变僵尸、任何子进程从不带 `--crashpad-handler-pid`、被杀 renderer 的父亲不是浏览器
进程、fd 数在 `FD_SLACK`（默认 40）以内——结尾和峰值都算——、页面两次被杀之间都还在回答
`js`、恰好出现两行 `renderer exited status=1 error=9`、demo 退出时没有进程留下。RSS 报告
但不钉死——它是锯齿。实测一次：40 s，19 个样本，fd 379 → 378（峰值 386），898 MB，20 次
读回答了 18 次，零遗留。`docs/internals/sandbox-policy.md` §7 和 §8 是这些数字不烂掉的方式。

`test/pixel-gate.sh` 是唯一看像素而不是日志的门禁。其他每个门禁读的是滚动位置、查找计
数、菜单模型——那些说明页面活着，没有一个说明它的像素到了。`test/pixel-page.html` 用四
块纯色填满视口，demo 把 view dump 一次，门禁逐字节比对每个象限的中心与 CSS 请求的 rgb，
合成 widget 和它所合成的 client texture 两边都比对。buffer 精确回来
`18,52,86 / 200,50,60 / 43,138,62 / 232,232,232`，且两块显示器上都如此：在
`./test/ci-session.sh` 里和在这台笔记本的真桌面上，12 条断言两边都无 skip。这也是这道门
值得存在的理由。它刚写出来时在真显示器上读的是 `#101828`——Chromium 清了 buffer 却从没
画过——页面自己的 `requestAnimationFrame` 计数器在 4.5 s 里停在 4 次回调，而这个差别被
归档为"没人看的窗口"的属性，配了相应的 skip。那是组件的回归：`bcbebc8` 把 frame
callback 从时钟路径上拿掉了，于是任何时钟会应答的显示器都不再应答它的 client。把调用放
回去，同一次运行从 4 帧动画变 252 帧、dump 从 `#101828` 变页面自己的绿色，skip 也从门禁
里拿掉了，因为一块平的 buffer 是关于组件的陈述，不是关于谁在看的。
`docs/internals/frame-clock-pacing.md` §8 和 §9 保留着整个往返，包括这道门本可抓住的冻结和它差点
因为被 skip 而漏掉的那次。

`test/ci-session.sh` 是唯一不在坐在这里的人的桌面上开窗口的门禁 runner。它起一个嵌套
compositor——无头模式的 GNOME Shell 48，即没有输出设备的 mutter——装在它自己的 HOME、
自己的 XDG 目录和自己的 D-Bus bus 里，对着那个显示器跑交给它的任何东西：

```bash
./test/ci-session.sh                        # 证明：窗口映射、页面加载
./test/ci-session.sh ./test/find-gate.sh    # 任何门禁，原样
```

这份隔离不是洁癖：对着真 HOME 起的嵌套 shell 加入了宿主的 session bus，激活了 nautilus、
Tracker miner 和 app-grid autofit，于是无人值守的运行动了用机器那个人的桌面。这个
compositor 是自己的 session leader，所以向它的进程组发一个信号就把 bus、portal 和 gvfsd
一起带走，运行还断言没东西留下。它证明的是：真窗口映射、真 Chromium session 开着沙箱
加载页面并回答 JavaScript、一个没法显示页面的脚本化运行在几秒内失败并指名原因——
helper 在 ready 之前死、连接都没建立就死、自己关掉浏览器、或根本没有 helper 可起（那样
连 `::closed` 都没有，脚本引擎自己说，`docs/features/session-lifecycle.md` §4–§6）——以及已有
门禁在它里面跑：`find-gate.sh`（34 条断言）、`permission-gate.sh`（58）、
`download-gate.sh`（54）、`dnd-gate.sh`（59）、`pixel-gate.sh`（12）、`primary-gate.sh`
（2）、`multi-browser-gate.sh`（6）、`sandbox-soak.sh`（12）和 `meson test`（15/15）全部
原样通过，`menu-gate.sh` 那次过了 47 条里的 45 条——skip 的两条断言的是弹窗 surface 到
了屏幕上，没有输出设备的显示器做不到（`docs/testing/headless-ci.md` §5）。最后一行截至
2026-10-22 在这台笔记本上不可复现：无头 shell 不再回答弹窗的 `xdg_surface.configure`，
于是 `gtk_popover_popup()` 在一个 Wayland roundtrip 里阻塞 demo 主线程，运行死在超时上
——在 `HEAD` 上把当前改动拿掉实测相同，真显示器上干净，所以是外层 compositor 而不是这
一层（`docs/testing/headless-ci.md` §6.1）。它证明不了的是帧节奏：没有输出设备的显示器从不跑
第二个 GTK 显示周期，而这正是 `docs/internals/frame-clock-pacing.md` §8 记录的故障模式——等那个
周期的 client 停止动画也停止绘制——还有 §9，那里的修复悄悄冻结了时钟会应答的那些显示
器。`docs/testing/headless-ci.md` 是记录。

`test/ci-all.sh` 是一条命令跑完整套电池——证明、`meson test` 和全部九个门禁，各在自己的
嵌套 compositor 里，末尾一张判定表、skip 的检查单列一栏、有失败就非零退出。这台笔记本
2026-10-21 实测：九次运行 370 s——273 条门禁断言加 `meson test` 14/14，2 条检查 skip。

```bash
./test/ci-all.sh                    # 全部
./test/ci-all.sh find menu          # 只要这两个
CI_ALL_REAL=1 ./test/ci-all.sh real # 外部站点，可选
```

## 当库用

`meson install` 把 `libcefnest.so` 放进 `libdir`、三个头文件放进 `includedir/cefnest`
（`nested.h`、`chromium-view.h`、`chromium-session.h`）、`cefnest.pc` 放在旁边，宿主用
`pkg-config` 构建：

```bash
cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

住在 prefix 外的二进制仍然需要 `LD_LIBRARY_PATH` 或一条 `ldconfig`——装好的 demo 不需
要，因为它的 `DT_RUNPATH` 是 `$ORIGIN` 相对的（`docs/guides/packaging.md` §4）。

```c
GtkWidget        *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession  *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
if (!chromium_session_start (session, "https://example.com", &error))
  g_printerr ("%s\n", error->message);
```

`ChromiumSession` 的信号：`::changed`（uri / 标题 / 加载中 / 历史变了，静音落地也算）、
`::ready`（helper 答了握手）、`::load-end`（uri、HTTP 码——失败的加载也发，码为 0）、
`::load-error`（网络错误码、uri、名称）、`::renderer-exited`（终止状态、错误码、消息）、
`::script-result`（id、ok、JSON 值、消息）、`::download-begin`（id、url、建议名、mime、
总字节数）、`::download-progress`（id、已收、总、百分比、速度）、`::download-end`（id、
`complete`/`cancelled`/`interrupted`、路径、已收）、`::find-result`（搜索 id、计数、活动
序号、final）、`::context-menu`（点击的 x、y 和作为 `GList` 的模型行——返回 TRUE 表示宿
主自己画）、`::permission-request`（helper 的 id、`media` 或 `prompt`、请求来源、权限名
`GList` 和原始掩码——返回 TRUE 表示宿主来答）、`::permission-dismissed`（id、result：
Chromium 从宿主手里收回的那个请求）、`::zoom-changed`（缩放命令或主框架加载后 helper 读
回的 CEF 缩放级别；0.0 是 100 %，0.5 是一个 Chrome UI 档）、`::primary-selection`（推给
宿主的页面选中文字，宿主把它写进 Gtk 主剪贴板）、`::copy-text`（从链接/图片/媒体菜单行
选出的 URL——剪贴板归宿主）、`::open-url`（从新窗口菜单行选出的 URL——demo 向共享
helper 要一个独立浏览器、给它自己的 GTK 窗口；库宿主想怎么路由都行）、`::page-message`
（页面 `cefnestPostMessage()` 的 payload 作为一个不透明 JSON 字符串——里面的 schema 归
宿主；这条 relay 两端都不封大小，所以超大的 payload 就是一条普通事件行、归宿主的 4 MiB
行上限管——`docs/guides/control-channel.md`）、`::popup-requested`（`target=_blank`、
`window.open` 或中键的目标 URL——**信号里只有 URL**：处置方式和手势留在 socket 事件上，
宿主从回调里分不出后台标签和前台窗口，而且 helper 已经替它做了决定）、
`::browser-created`（id、url、`windowed`：TRUE 表示这个浏览器有自己的宿主窗口而不是共享
view 里的又一个标签）、`::browser-selected`（现在拥有共享 view 的 id）、
`::browser-closed`（消失了的 id——windowed 浏览器会来，**标签永远不会**，因为 helper 的
close 在那里是静默空操作，见"已知限制"）、`::closed`（helper 没了且不再回来，四种原因
之一：宿主停了它、helper 关了浏览器、通道挂了、进程被收走——一次死亡报后两者之一，所以
无人值守的运行说的是*那个*而不是猜）。
`chromium_session_set_muted ()` 让页面安静——它的声音从不带沙箱的 audio 进程离开 helper、
落到宿主的扬声器上，静音时流保持打开，所以桌面混音器看不见这个开关
（`docs/features/audio-path.md`）。`chromium_session_zoom (session, "in"/"out"/"reset")` 以半级为
步改变页面的 CEF 缩放级别；Chromium 按来源持久化级别，helper 用 `::zoom-changed` 回答而
不是让宿主猜改动有没有落地。`chromium_session_get_zoom_level ()` 是它报告过的最后一个级
别。demo 绑定 Ctrl+plus、Ctrl+minus 和 Ctrl+0。下载是一个问题而不是命令：宿主用
`chromium_session_accept_download (session, id, path)` 回答 `::download-begin` 或用
`chromium_session_reject_download (session, id)` 拒绝之前，磁盘上什么都不会有；不回答的
那个也一个文件都不留（`docs/features/download-path.md`）。查找是同一问题的另一个形状：
`chromium_session_find (session, pattern, forward, match_case, next)` 不回答任何东西，
`::find-result` 是唯一的回复，所以宿主只在 `final` 到达时渲染总数
（`chromium_session_stop_finding ()` 结束它，它回得更安静——什么都不回）。demo 把两者接
到 Ctrl+F 开、Esc 关的查找栏上；按键控制器跑在 capture 阶段，因为 `GtkEntry` 否则会先于
view 的转发拿走 Esc（`docs/features/find-path.md`）。右键是第三个形状，也是必须回答的两种之一：
CEF 交出自己的菜单模型并期待恰好一个回复，回复悬着期间浏览器不会再问——五次不回答的
右键是一个事件。`::context-menu` 返回 TRUE 接管菜单，随后恰好一次
`chromium_session_context_menu_select (session, id)`（执行该 id 对应的 CEF 自己的命令
——全选实测为在页面里选中）或 `chromium_session_context_menu_dismiss (session)`。返回
FALSE 或什么都不连，session 自己把模型画成 view 上的 popover，所以右键永远不会是死手势
（`docs/features/context-menu.md`）。权限请求是第四个形状、同一份"页面站在答案上"的契约：不回答
的 `getUserMedia` 让 promise 挂着**并且**压住它后面的媒体队列，不回答的 prompt 在
Chromium 显示下一个的一刻被当作 `ignore` dismiss。
`chromium_session_permission_respond (session, id, result)` 送出答案，`accept` 意思是"整
个掩码"——CEF 要求授予的掩码等于请求的掩码，所以宿主给不了"麦克风但不给摄像头"；半个
授予就是拒绝。返回 FALSE 或什么都不连，session 在到达时就拒绝，而不是让页面干等
（`docs/features/permission-prompt.md`）。`docs/guides/control-channel.md` 是完整的命令与事件清单，以及
被决定不做的那些命令的记录。

## 细节在哪里

`docs/` 放的是各种记录——中文，带实测的前后证据。它们是"代码为什么长成这样"的档案：

| 文档 | 关于 |
| --- | --- |
| `docs/internals/nested-split.md` | `nested.c` 怎么按协议拆成模块 |
| `docs/internals/frame-clock-pacing.md` | 按 GTK 帧时钟而不是定时器绘制、给永远不来的显示周期配的看门狗，以及发现看门狗悄悄把时钟路径从帧上拿掉的那一轮 |
| `docs/internals/popup-compositing.md` | 页面弹窗的 surface 栈、摆放与关闭 |
| `docs/features/clipboard-bridge.md` | `wl_data_device` ↔ `GdkClipboard` 双向 |
| `docs/features/session-lifecycle.md` | 与 helper 一起启动、停止和死亡 |
| `docs/internals/resource-limits.md` | 每 pool 和每连接的 shm 上限、分配检查、dmabuf fd 泄漏 |
| `docs/internals/empty-request-slot.md` | 一个 NULL 请求槽位杀死整个桌面 |
| `docs/features/wheel-resolution-and-key-repeat.md` | `axis_value120`、每个手势一帧、自动重复的记账 |
| `docs/internals/clipboard-offer-during-client-init.md` | 一个合法却早一次 dispatch 发出的 `data_offer` 杀死一个 GTK client |
| `docs/internals/sandbox-policy.md` | 沙箱决策：不需要 setuid helper、没有用户命名空间的宿主对运行意味着什么、让进程树和 fd 数在反复导航与两次 `kill -9` renderer 之间保持平稳的 soak（即 `test/sandbox-soak.sh`），以及为什么这里没有崩溃处理器可修可关 |
| `docs/guides/control-channel.md` | socket 上的每条命令与事件、失败的加载和死掉的 renderer 长什么样、被决定不做的命令 |
| `docs/features/zoom-path.md` | CEF 缩放级别、半级步长、按来源持久化、`load-end` 为什么重报级别、demo 的键位 |
| `docs/testing/real-sites.md` | 五站点真实页面巡检批次：每站一个嵌套 session、截图、JS 读回、3 秒 rAF 计数，以及实测的 H.264/VP9 结论 |
| `docs/features/pdf-path.md` | 实测的 PDF 查看结果：widget dump 里的缩略图/翻页控件，以及没测的东西（表单、注释、打印） |
| `docs/features/multi-browser.md` | 单 helper 的 browser 注册表、标签条、独立宿主窗口、协议 id、按窗口的输入交还规则及其门禁，以及剩下的弹窗警告 |
| `docs/testing/cnb-ci.md` | cnb.cool 入口、它期望的自托管 runner、一次性的原生/CEF 前置 |
| `docs/structural-decisions.md` | 有意留给产品决策的两件事：标签/窗口的浏览器路由，以及 arm64/loong64 的 CEF 供给 |
| `docs/internals/frame-metrics.md` | compositor 帧计数器 / 脚本与环境导出，以及软件路径的 pool mmap + damage 影子拷贝 |
| `docs/guides/minimal-host.md` | 三个对象的宿主、必需的顺序、导航/JS/下载/菜单/主选区/无障碍该加哪个 API/信号的对照表，以及 §5：中等示例、它的脚本化运行测到了什么、关不掉的那个标签 |
| `docs/features/audio-path.md` | 页面的声音实际走哪：一个不带沙箱的 audio 服务直通宿主扬声器，以及静音测到什么 |
| `docs/internals/protocol-ceilings.md` | 真实 client 对嵌套 compositor 绑定什么，以及每个未通告的协议与版本上限的实测判定 |
| `docs/features/download-path.md` | 停靠的 `OnBeforeDownload` 门、谁定路径，以及只有测量才能告诉你的四种 CEF 下载行为 |
| `docs/features/find-path.md` | 一条不回答的命令、只有测量才给的回复形状（每请求一个 identifier、每请求一到三次更新），以及真正让页面滚动的东西 |
| `docs/features/drag-loop.md` | 页面内拖拽的编排、"wl_data_device.enter 到得了页面"的实测证明、目的地真正接受什么、为什么 fd 中继不是同页 payload 的载体、跨越窗口边缘的拖拽怎么从页面的 source 上读出来交给宿主（图片字节落成以文件列表提供的临时文件）、反方向的同一跨越怎么让 compositor 当 source——包括"页面在被 enter 的一刻就读宿主 drag"那条测量，所以只有 `finish` 能定判定 |
| `docs/features/context-menu.md` | 决定整个设计的唯一约束（Chromium 只握一个未回答的菜单请求）、CEF Alloy 默认模型里到底有什么、helper 加的链接/图片/媒体行、以及 `Continue(id)` 在页面内被证明 |
| `docs/features/permission-prompt.md` | 哪些请求根本到不了宿主、让一个不回答的请求连累页面下一个问题的两条队列，以及为什么 `accept` 只能是"整个掩码" |
| `docs/guides/packaging.md` | 什么装到哪、session 找 helper 的五步规则、为什么 `get_option('libexecdir')` 必须先拼进 prefix 再编进去、两个方向故意相反的回退 |
| `docs/testing/headless-ci.md` | 对着没人看得见的嵌套 mutter 跑门禁：为什么 session 要自己的 HOME 和 bus、一个信号怎么收走整棵进程树、没有输出设备的显示器能证明什么不能证明什么 |
| `docs/features/a11y.md` | 读屏器能否够到嵌套架构里的页面：能，因为 Alloy 的 windowed 浏览器在 `STATE_ENABLED` 就是 Chromium 自己的 Complete AT-SPI 模式；切换它的 session-bus 监听器，以及 ORCA 看到了什么 |
| `docs/internals/dmabuf-formats.md` | feedback 格式表由本进程能导入什么——GDK 通告的集合——决定，而不是静态四格式通告，附带对 CEF 全线性 buffer 的实测后果 |
| `docs/features/fullscreen.md` | Alloy 在 `requestFullscreen()` 上只*通知*——触发迁移的是 client；从 `OnFullscreenModeChange` 到 `xdg_toplevel.set_fullscreen` 的接线，以及实测的改前 |
| `docs/internals/map-cycle.md` | 每次 map 挂的东西都得有拆下的路（M11 发现：N 次 map 周期，N 份 handler）；守住这条规则的两个门禁及各自被验证过的变异 |

## 已知限制

- **拖出与拖入都已端到端可用，手工验证。** 一次在同页面里起止的拖动跑完整套
  `wl_data_device` 协商，页面自己的拖影合成在最上面。向外，compositor 从页面自己的
  source 上读走 payload、叫停那个拖动，把 mime、字节和类型列表交给宿主，宿主在 toplevel
  的 native surface 上起一个 `GdkDrag`（`docs/features/drag-loop.md` §6）。文本按文本携带；
  Chromium 的图片拖动以 `application/octet-stream;name="download.png"` 到达，demo 于是写
  临时文件并提供 `GdkFileList`，文件管理器能复制的就是它。向内，view 上的
  `GtkDropTarget` 接受文件列表和字符串：enter 时通过 `GdkDrop` 预取 payload 字节（上限
  32 MiB），compositor 把它们作为 `wl_data_offer` 引入，页面协商并读取，判定在合成 drop
  上回来（`docs/features/drag-loop.md` §7）。两个方向都过了真手指的拖动（文本拖出到编辑器、从它
  拖入）。
- **一个 seat、一个指针、没有触摸。** 嵌入 widget 是 compositor 认识的唯一设备。
- **独立宿主窗口拿得到输入，有门禁作证。** 用 `nested_compositor_set_input_surface()`
  点名的 view 拿走指针和键盘，交还是显式的而不是自愈的：GTK 窗口关闭或隐藏的 view 调用
  `nested_compositor_clear_input_surface()`，它只在那个 view 正是被点名的那个时才撤销覆盖
  ——这样后关的第二个窗口剥不走此后接管的窗口的路由——并在同一次调用里 retarget seat，
  而不是等下一个事件。compositor 相信的随后由 client 画的来核对，用的是
  `seat-selftest --second-window`（`docs/features/multi-browser.md`）。仍有一条路径在这个保证之外：
  在独立窗口里**发起**的拖动。出界交接把它的 `GdkDrag` 建在 demo 的主 toplevel 上
  （`docs/features/drag-loop.md` §6），所以这样的拖动从错误的 surface 出发。
- **标签模式的浏览器关不掉，请求被无声丢弃。** 对 windowed 浏览器，
  `chromium_session_close_browser()` 跑完整套实测序列（`browser N closing` →
  `browser N closed` → `browser 1 selected`）；对标签，helper 摘下 view 并调用
  `CloseBrowser`，之后什么都没有——没有 `OnBeforeClose`，于是也没有 `::browser-closed`。
  不存在的 id 会回答 `status: no-such-browser`，宿主靠它分"已经没了"和"被无声拒绝"
  （code-review-2026-10-22.md M22；三方探测在 `docs/features/multi-browser.md` §1 和
  `docs/guides/minimal-host.md` §5）。同一个不对称早一个信号就有：`::popup-requested` 只带 URL，
  所以宿主看不见页面要的是后台标签还是前台窗口——那在 socket 事件的 `disposition` 上，
  信号没有转发它。
- **这个 CEF 发行包里没有 H.264/MP4。** 真 YouTube 报 `videoReady=4`，但
  `canPlayType('video/mp4; codecs="avc1.42E01E"')` 为空；VP9 WebM 答 `probably`，
  YouTube 仍以 VP9 播放。MP4/H.264-only 的页面会失败，直到发行包做出编码决策。实测在
  `docs/testing/real-sites.md` §3.1。
- **没有双指缩放，没有 tablet 输入。** Chromium 对真 compositor 绑定
  `zwp_pointer_gestures` 和 `zwp_tablet_manager_v2`；嵌套 compositor 不通告它们，所以页面
  恰好失去这两个特性、别的什么都不缺（`docs/internals/protocol-ceilings.md`）。主选区（中键粘贴）
  另有桥接，仅文本（`docs/features/clipboard-bridge.md` §5）。
- **失败的导航报两次，`::load-end` 照发。** 先是真的错误码
  （`ERR_NAME_NOT_RESOLVED`），约一秒后再来一个 `ERR_ABORTED` (-3)；load-end 的 HTTP 码
  是 0。什么都不过滤，因为"宿主该看哪个错误"不是这一层能替它决定的
  （`docs/guides/control-channel.md`）。
- **只有 renderer 的死被报告，且只报告怎么死的。** `::renderer-exited` 来自 CEF 的渲染
  进程回调——`kill -9` 实测为 `status=1`、错误码 9，这是 OS 级的 wait 状态，仅此而已：
  minimal CEF 发行包不带崩溃处理器，这棵树里没有任何进程带 `--crashpad-handler-pid`，
  所以没有 minidump 也没有栈（`docs/internals/sandbox-policy.md` §8）。其余被沙箱的子进程死掉时，
  宿主要等到页面不再应答才知道。
- **下载可以接受、拒绝、取消、暂停/恢复。** `CefDownloadItemCallback::Pause/Resume` 接为
  `download-pause` / `download-resume`；终止事件仍是 CEF 的 `download-end`。三个终态里
  `interrupted` 是 `test/download-gate.sh` 够不到的那个：本地服务器一直答到 Chromium 自己
  放弃（`docs/features/download-path.md`）。而 `complete` 不是文件出现的时刻：字节在门禁还停靠
  着的时候就到齐的话，路径一指定文件就在盘上了，CEF 的终止事件 3–6 s 后才来，与
  Chromium 的下载数据库落盘批次一起（实测 3.24 / 4.12 / 5.72 s，所以门禁等 9 s——
  `docs/features/download-path.md` §4.4）。
- **查找一步一步走匹配。** CEF 的 `Find` 没有"跳到第 N 个匹配"，全新搜索一开始什么都不
  选（活动序号为 0），所以想要第五个匹配的宿主发五步。三跑之一实测过第一步报了第三个
  匹配——序号是 Blink 自己的排序，门禁刻意不钉死它（`docs/features/find-path.md`）。
- **helper 扩展右键菜单；宿主仍然不能。** 右键以模型到宿主、以命令 id 回去。helper 从
  `CefContextMenuParams` 加链接/图片/媒体行（在新窗口打开、复制地址、保存图片）并执行它
  们；想要自己那几行的宿主仍然需要这些参数过通道，那是下一步协议工作。图片保存现在用
  `GtkFileDialog` 问路径；`CEFNEST_DOWNLOAD_NO_SAVE_AS` 退出它、回到下载目录。勾选和单选
  标记不画、子菜单行打不开——菜单在每条命令后从新模型重建，标记没有可记的东西。
- **权限只能给或拒，不能收窄。** 媒体请求必须以它要的精确掩码回答——`Continue()` 要求
  `allowed_permissions` 与请求一致——所以"麦克风行、摄像头不行"在这里不存在。宿主晾着的
  一个请求代价不止沉默：页面的 promise 永不 settle、媒体队列压在它后面，且实测两次，紧跟
  在被晾 prompt 后面的那个请求凭空消失、从未到达宿主。三种请求根本到不了宿主
  （`navigator.storage.persist()`、`permissions.query()`、idle 检测——它们不走
  `CefPermissionHandler`），所以没有宿主策略能覆盖它们，请求之间也不记任何东西：同一个
  权限问两次就到 handler 两次（`docs/features/permission-prompt.md`）。
- **页面的声音完全绕开 widget。** 音频活在一个不带沙箱的 `audio.mojom.AudioService`
  utility 里，像普通桌面应用一样到达宿主扬声器；嵌套 compositor 从来看不见它，GNOME 的
  混音器分不出静音的流和安静的流。`set_muted` 是唯一的开关，也没有任何东西宣布页面
  *开始*出声（`docs/features/audio-path.md`）。
- **renderer 被沙箱；helper 本身没有，也不可能。** 浏览器进程握着 Wayland 连接、GPU
  buffer 和控制 socket，所以它按设计保持无沙箱——信任边界是 renderer 里的页面代码，不是
  `cefnest-helper`。在没有非特权用户命名空间也没有 setuid `chrome-sandbox` 的宿主上，
  helper 启动即死而不是悄悄无沙箱地跑；故意这样说的方式是
  `CHROMIUM_HELPER_NO_SANDBOX=1`（`docs/internals/sandbox-policy.md`）。
- **只有原生 Wayland，这是设计。** X11 session 下 GDK 给不了 widget 一个可交给 client 的
  Wayland surface，组件在那里不工作。
- API 不稳定。它*确实*被安装——`src/meson.build` 构建 `libcefnest.so.0.0.1`，旁边带
  `cefnest.pc`——但没人等一次 soname bump：一个信号可以加参数（`::closed` 就加过），
  一个头文件可以在版本之间变形，所以宿主链接的是它自己构建的那份。

## 许可

LGPL-2.1-only——见 `LICENSE`。Chromium helper 链接 CEF，CEF 是 BSD-3-Clause 并带自己的第三方声明；
那些不是本仓库的一部分，也不由本仓库再分发。
