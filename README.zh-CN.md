# CEFNest

[English](README.md) | 简体中文

CEFNest 是一个 GTK 4 组件，用于显示由**真正的 Chromium** 渲染的网页——通过一个运行在同一
进程内的 Wayland compositor 将页面合成进该组件。

一切都由两条约束决定：

- **原生 Wayland。** 不用 XWayland，不做 X11 client，没有 `XEMBED`。页面由 Chromium 自己的
  `ozone/wayland` 后端绘制。
- **不用 OSR。** 不存在把"离屏渲染"buffer 拷进 Cairo surface 这一步。Chromium 创建一个
  Wayland surface，我们的 compositor 像任何 compositor 一样接收它，并以 `GdkTexture` 的形式
  绘制进 widget。

这就排除了所有不自带 compositor 的"把浏览器嵌入 GTK"方案，因此本仓库必须自带一个。

## 工作原理

```
 GTK 4 应用
   └── ChromiumView（一个 GtkWidget）
         └── NestedCompositor            src/nested*.c — 一个 libwayland-server compositor
               · 在 $XDG_RUNTIME_DIR 中创建自己的 socket
               · 接收 shm / dmabuf buffer，合成 surface 栈
               · 将 widget 真实的指针、键盘与输入法转发给 client
         ▲
         │ Wayland 协议（socket 的 client 一侧）
         │
 cefnest-helper                          helper/ — 一个 CEF 应用，独立进程
   · 以 WAYLAND_DISPLAY=<嵌套 compositor 的 socket> 启动
   · Chromium 将页面渲染进那个 compositor
   · 控制面：unix socket 上的 NDJSON，双向
```

三个组件各司其职：

| 组件 | 文件 | 职责 |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | compositor：globals、surface、damage、输入、剪贴板、跨窗口边界的双向拖放 |
| `ChromiumView` | `src/chromium-view.c` | widget：把 GdkSurface 的输入交给 compositor，绘制合成结果 |
| `ChromiumSession` | `src/chromium-session.c` | 控制面：启动/停止 helper、跟踪页面状态、执行 JavaScript、代理下载、查找、页面右键菜单与权限请求 |

compositor 通告的 globals 及版本见 `src/nested-internal.h`：`wl_compositor`、`wl_shm`、
`wl_seat`（8，因此 pointer 和 keyboard 可以携带 `axis_value120`）、`wl_output`、
`wl_data_device_manager`（3）、`xdg_wm_base`、`zwp_linux_dmabuf_v1`（5，带 feedback）、
`wp_viewporter`、`wp_fractional_scale_manager_v1`、`zwp_text_input_manager_v3`、
`wp_cursor_shape_manager_v1`。

## 依赖

在开发机上实测（Debian 13、`linux 6.12`）：

| 软件包 | 实测版本 | 最低 |
| --- | --- | --- |
| gtk4 | 4.18.6 | 4.14 |
| wayland（server + client） | 1.23.1 | 任何带 `axis_value120` 的版本（1.21+） |
| wayland-protocols | 1.44 | 须包含 `staging/cursor-shape`、`staging/fractional-scale`、`unstable/text-input` |
| xkbcommon | 1.7.0 | — |
| gbm / libdrm | 25.0.7 / 2.4.124 | — |
| json-glib | 1.10.6 | — |
| meson / ninja / cmake | 1.7 / 1.12 / 3.16+ | meson 1.3（demo 的 `$ORIGIN` rpath 用 `fs.relative_to` 构建） |
| CEF（`cef_binary_*_linux64_minimal` / `linuxarm64_minimal`） | 已验证的 stable 154.0.34+g14c5a08+chromium-154.0.8037.98 | 任何能用其自带 CMake helpers 构建的版本；arm64 的 URL 路径已接好，但尚未在 arm64 主机上测试 |

`wayland-scanner` 必须在 `PATH` 上（协议代码在构建期生成——仓库不提交生成产物）。

渲染一页还需要 Chromium 沙箱所需的东西：可用的非特权用户命名空间。开发机上有
（`unprivileged_userns_clone=1`，没有 AppArmor 限制）；禁掉它的宿主是装包侧的问题，
写在下面**安装**一节和 `docs/internals/sandbox-policy.md` §6。

## 构建

分两个阶段，第一阶段可选。

```bash
# 1. 拉取已验证的 stable CEF 二进制发行包（gitignore，下载约 500 MB，
#    解压后 1.5 GB）；CEF_VERSION=latest 取最新 stable
./scripts/fetch-cef.sh

# 2. Chromium 侧 —— 自动发现 third_party/cef/dist
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. widget、demo、测试
meson setup builddir
ninja -C builddir
```

`scripts/fetch-cef.sh` 默认取已验证的 stable 154.0.34；`CEF_VERSION=latest` 取 CEF
index 当前标为 stable 的最新构建，`CEF_VERSION=<完整 CEF 版本>` 固定其它构建，
`CEF_CHANNEL=any` 取任意 channel 的最新构建。换发行包也可用 `-DCEF_ROOT=…` 指定。
一次 CEF 版本升级意味着要重跑整套门禁——一次 Chromium 里程碑升级会同时改变 client
请求哪些 Wayland 协议（`docs/internals/protocol-ceilings.md` 是重测记录）。

跳过第一阶段仍可正常构建：此时 demo 会改用 `test/shm-client` 而非 Chromium，这已足以覆盖
compositor、seat、剪贴板桥和弹窗。

## 安装

本组件以共享库形式安装，附带 pkg-config 文件、头文件和 demo；`docs/guides/packaging.md`
记录了每一样装到哪里，以及 session 如何决定启动哪个 helper 二进制。

```bash
meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install
```

| 选项 | 默认 | 决定什么 |
| --- | --- | --- |
| `helper-path` | 空，由 `prefix` + `libexecdir` + 主机架构推导 | 编进库里的路径，无人指定时使用 |
| `helper-build-dir` | `helper/build/Release` | cmake 的构建产物：开发期回退路径，也是被安装的对象 |
| `install-helper` | `false` | 同时安装 helper 和整套 CEF 运行时——约 1.5 GB，因此是可选项 |
| `install-demo` | `true` | 安装 `cefnest-demo`，以及无 Chromium 时它回退到的 `shm-client` |

在未跑过 cmake 的构建树上开启 `install-helper` 会在 configure 阶段报错，而不是留下一个空目录、
配一行看似成功的 "Installing subdir"。安装出去的 helper 对构建它的树一无所需——CEF 运行时
随它一起安装，并从它所在目录解析，这是把整个 `third_party/cef/dist` 目录挪走之后实测过的
（`docs/guides/packaging.md` §9）。

CNB 的发版流水线把安装树打成两个包，各有 deb 与 rpm 两种格式。`cefnest` 就是
组件本身：GTK 4 部件库、`cefnest-helper` 和整套 CEF 运行时（安装后约 1.5 GB）。
它以系统库 `libcefnest.so.0` 装进发行版，只依赖系统库，装它就能运行宿主。
`cefnest-dev` 放头文件、链接用的 `.so` 与
`cefnest.pc`，精确依赖对应版本的 `cefnest`；装好后用
`pkg-config --cflags --libs cefnest` 构建宿主。demo 与 `shm-client` 是开发测试工具，
不打包，只随 `cefnest-build.tar.gz` 发布。两个包共用一个版本号——CEF stable `X.Y.Z`
加修订号（`154.0.34-7`）——同一次构建一起发布。从 -6 的四包布局升级由 deb 的
`Replaces`/`Breaks` 与 rpm 的 `Obsoletes` 处理，不需要手工卸载旧包。流水线只在
版本 tag 上构建，详见 `docs/testing/cnb-ci.md`。

`cefnest-helper` 默认**开着**沙箱启动 Chromium，而它把 `chrome-sandbox` 原样装成 0755
——没有 setuid 位，也不带 AppArmor profile。宿主有可用的非特权用户命名空间时，事情到
此为止：没有安装步骤，不需要 root。宿主禁掉它时（Ubuntu 24.04 默认打开
`apparmor_restrict_unprivileged_userns`；这个开关 23.10 就有）装好的 helper 会停在
`No usable sandbox!`，把这句话打到 stderr，绝不静默降回无沙箱。两条出路：给
`cefnest-helper` 单独加一条允许 userns 的 AppArmor profile，或在宿主环境里设
`CHROMIUM_HELPER_NO_SANDBOX=1`。Chromium 自己那句提示里的 `chmod 4755 chrome-sandbox`
不是本项目发行或实测过的路。写清楚的是 `docs/internals/sandbox-policy.md` §6；
`scripts/ci-smoke-packages.sh` 每次发版断言那个 0755，并把装好的 helper 真起一次。

## 运行

```bash
# 正常运行 —— cmake 构建过 helper 后无需任何环境变量，
# 因为库自己能找到它（docs/guides/packaging.md §2）
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/src/cefnest-demo

# 不涉及 Chromium：指定一个不存在的 helper，demo 会回退到
# 通过嵌套 compositor 驱动 test/shm-client
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo

# 已安装的版本，从 prefix 启动，无需任何环境变量
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest $HOME/.local/cefnest-stage/bin/cefnest-demo
```

`CHROMIUM_HELPER` 仍然是"设了任意值即强制 session 模式"，`test/fake-helper.py` 就是这样在
没有任何浏览器的情况下被脚本驱动的。

demo 的全部配置来自环境变量，因为它的目的就是可复现的运行。核心几项：

| 变量 | 作用 |
| --- | --- |
| `CHROMIUM_URL` | 要打开的页面 |
| `CEFNEST_HELPER` | 含 `/` 的路径优先于 `CHROMIUM_HELPER`；两者都没设时，库自行解析 helper（`docs/guides/packaging.md`） |
| `CHROMIUM_HELPER` | 设了（任意值）即强制 session 模式；这里的路径必须含 `/`，且按原样使用——即使不存在 |
| `CHROMIUM_SESSION_SCRIPT` | 一行一条命令的脚本文件（`load`、`js`、`find`、`menu-pick`、`download-accept`……）；完整的动词清单以及"哪些步骤需要 helper session"见 `docs/testing/overview.md` |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | N ms 后抓取 widget 写一张 PNG 然后退出 |
| `CHROMIUM_HELPER_NO_SANDBOX` | 恰好为 `1` 才不带 Chromium 沙箱运行 helper；其它任何值——包括手滑打错——都保持开启（`docs/internals/sandbox-policy.md`） |
| `CHROMIUM_USER_AGENT` | 随 helper 启动烙进 Chromium 的默认 User-Agent；宿主在 `chromium_session_start()` 前放进自己的环境即可——`examples/cefnest-useragent` 演示运行中重启切换 |
| `G_MESSAGES_DEBUG` | **空格分隔**的域：`all`，或 `nested chromium-view chromium-session`——逗号列表什么都匹配不到 |

其余的——下载/权限/拖动的面板模式、剪贴板与主选区预置、无障碍、拼写检查、帧指标、窗口
几何——随门禁一并记录在 `docs/testing/overview.md`。

## 最小宿主

`examples/minimal.c` 是只用三个对象的示例——窗口、`ChromiumView`、`ChromiumSession`——
不带任何 demo 外壳；`CEFNEST_URL` 指定它打开的页面。`examples/host.c` 是在同样三个对象
基础上再进一步的示例：页面的某个操作必须由宿主回答的那四个信号——`::download-begin`
（于是页面文件落到哪里完全由宿主决定）、`::permission-request`、`::script-result`、
`::browser-created`——通过脚本文件驱动，无需键盘即可测试（`CEFNEST_HOST_SCRIPT`，动词
`load tab select close js save drop allow deny wait quit`）。必需的启动顺序、生命周期、
导航/JS/下载/菜单/主选区/无障碍的 API 对照表，以及脚本化运行测到的结果，都在
`docs/guides/minimal-host.md`。

```bash
ninja -C builddir
CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-minimal
```

## 测试

```bash
meson test -C builddir                       # 25 个测试
meson test -C builddir --suite selftest      # 二十个 seat selftest + map-cycle
meson test -C builddir --suite protocol      # 三个源码审计
meson test -C builddir --suite unit          # helper 路径解析规则
```

树上其余的检查都是需要打开真实窗口的门禁，因此没有一个是 `meson test`。
`test/ci-session.sh` 可以把任何门禁跑在一个嵌套的无头 compositor 里——使用自己的 HOME、
XDG 目录和 D-Bus bus，不影响任何人的桌面；`test/ci-all.sh` 则是用这种方式跑完整套测试集的
唯一命令，每个门禁一个嵌套 compositor，末尾输出一张判定表（实测 370 秒：273 条门禁断言加
`meson test` 14/14）。

```bash
./test/ci-session.sh ./test/find-gate.sh     # 任何门禁，无头
./test/ci-all.sh                             # 全部
CI_ALL_REAL=1 ./test/ci-all.sh real          # 外部站点，可选
```

每个门禁证明什么、断言背后的实测数字、每条断言所验证的变异：见
`docs/testing/overview.md`。无头 runner 自己的记录是 `docs/testing/headless-ci.md`，
cnb.cool 入口是 `docs/testing/cnb-ci.md`，真实页面批次是
`docs/testing/real-sites.md`。

## 当库使用

`meson install` 会把 `libcefnest.so` 放进 `libdir`、三个头文件放进
`includedir/cefnest`（`nested.h`、`chromium-view.h`、`chromium-session.h`）、`cefnest.pc`
放在旁边，宿主用 `pkg-config` 构建：

```bash
cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

安装在 prefix 外的二进制仍然需要 `LD_LIBRARY_PATH` 或一条 `ldconfig`——装好的 demo 不需
要，因为它的 `DT_RUNPATH` 是 `$ORIGIN` 相对的（`docs/guides/packaging.md` §4）。

```c
GtkWidget        *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession  *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
if (!chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error))
  g_printerr ("%s\n", error->message);
```

session 的契约是：页面的某个操作必须由宿主来回答。下载、查找、右键菜单、权限请求都是
session 代为处理的问题，各有自己的回复形状，也各有不回答的代价。每个信号与调用、连同它
携带的实测形状，都写在 `src/chromium-session.h` 的内联文档中；底层通道的命令与事件清
单——以及被决定不做的那些命令——是 `docs/guides/control-channel.md`。

## 文档

`docs/` 放的是各种记录——中文，带实测的前后证据。`docs/README.md` 是索引：什么在哪、
先读哪篇。

## 已知限制

每条一行；带全部实测的完整记录是 `docs/known-limitations.md`。

- 拖出与拖入都已端到端可用，手工验证。
- 一个 seat、一个指针、没有触摸。
- 独立宿主窗口可以拿到输入，门禁已验证——但在其中发起的拖动会从错误的 surface 出发。
- `::popup-requested` 只带 URL，宿主分不出页面要的是后台标签还是前台窗口。
- 此 CEF 发行包不含 H.264/MP4；VP9 可播放。
- 没有双指缩放，没有 tablet 输入。
- 失败的导航会报告两次，`::load-end` 仍会触发。
- 只有 renderer 的死亡会被报告，且只报告死因。
- 下载可以接受、拒绝、取消或暂停/恢复——但 `complete` 不是文件出现的时刻。
- 查找按步逐个走过匹配项。
- helper 扩展右键菜单；宿主仍无法扩展。
- 权限只能授予或拒绝，不能收窄——且一个未回答的权限请求还会连累页面的下一次请求。
- 页面的声音完全绕过 widget；`set_muted` 是唯一的开关。
- renderer 受沙箱保护；helper 本身不受沙箱保护，也不可能受保护。
- 禁掉非特权用户命名空间的宿主上，装好的 helper 拒绝启动，直到你给它一条 AppArmor
  profile 或设 `CHROMIUM_HELPER_NO_SANDBOX=1`。
- 仅支持原生 Wayland，这是设计决定。
- API 不稳定。

## 许可

LGPL-2.1-only——见 `LICENSE`。Chromium helper 链接 CEF，CEF 采用 BSD-3-Clause 许可并附带自己的第三
方声明；这些不属于本仓库，也不由本仓库再分发。
