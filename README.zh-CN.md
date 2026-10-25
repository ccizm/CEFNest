# CEFNest

[English](README.md) | 简体中文

一个 GTK 4 控件，用**真正的 Chromium**（CEF）渲染网页——由同一进程内运行的
Wayland compositor 直接合成进控件。既不使用 XWayland，也不做离屏渲染：Chromium
画在自己的 Wayland surface 上，控件则像绘制普通内容一样把它画出来。

## 特性

- 只走原生 Wayland——使用 Chromium 自己的 `ozone/wayland` 后端，GPU 或 shm 均可
- 一个 helper 进程即可服务多个标签页和独立窗口
- 可由宿主应答的 API：下载、查找、右键菜单、权限提示、页面 ↔ 宿主消息、
  跨窗口边界的剪贴板与拖放
- 缩放、静音、无障碍（ORCA 可直接使用的 AT-SPI）、帧计数
- 右键"查看网页源代码"是组件自带的只读源码窗口：行号、语法高亮、查找、
  自动换行、复制；源码只在内存中流转，不再写临时文件、不再拉起外部编辑器

## 工作原理

```
 GTK 4 应用
   └── ChromiumView（一个 GtkWidget）
         └── NestedCompositor            src/nested*.c — 一个 libwayland-server compositor
               · 在 $XDG_RUNTIME_DIR 下拥有自己的 socket
               · 接收 shm / dmabuf 缓冲区，合成 surface 栈
               · 把控件真实的指针、键盘和输入法事件转发给客户端
         ▲
         │ Wayland 协议（socket 的客户端一侧）
         │
 cefnest-helper                          helper/ — 一个 CEF 应用，独立进程
   · 以 WAYLAND_DISPLAY=<嵌套 compositor 的 socket> 启动
   · Chromium 把页面渲染进那个 compositor
   · 控制平面：unix socket 上的 NDJSON，双向通信
```

| 部件 | 文件 | 职责 |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | compositor：globals、surface、damage、输入、剪贴板、跨窗口边界的双向拖放 |
| `ChromiumView` | `src/chromium-view.c` | 控件：把 GdkSurface 的输入交给 compositor，并绘制合成结果 |
| `ChromiumSession` | `src/chromium-session.c` | 控制平面：启动和停止 helper、跟踪页面状态、执行 JavaScript、中转下载与查找，并处理页面右键菜单和权限请求 |

## 依赖

| 软件包 | 最低版本 |
| --- | --- |
| GTK 4 | 4.14 |
| Wayland（server + client） | 1.21+（`axis_value120`） |
| wayland-protocols | 含 `cursor-shape`、`fractional-scale`、`text-input` |
| xkbcommon / json-glib / gbm + libdrm | 任意较新版本 |
| GtkSourceView | 5.4+（源码窗口） |
| meson / ninja / cmake | meson 1.3+，cmake 3.16+ |
| CEF（`cef_binary_*_linux64_minimal`） | 已在 stable 156.0.14 上验证通过 |

`wayland-scanner` 必须在 `PATH` 上。渲染还需要可用的非特权用户命名空间（Chromium
的沙箱依赖它）；如果系统限制了这一点，见
[docs/troubleshooting.zh-CN.md](docs/troubleshooting.zh-CN.md)。

## 构建

```bash
./scripts/fetch-cef.sh                # 1. 拉取已验证的 CEF 二进制发行版
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release   # 2. Chromium 侧
cmake --build helper/build -j
meson setup builddir && ninja -C builddir                    # 3. 控件、demo、测试
```

第 1 步会下载约 330 MB（解包后约 1.5 GB）。跳过它，其余部分照样能构建——此时
demo 会改为驱动一个协议级测试客户端，而不是 Chromium。

## 安装

```bash
meson setup builddir -Dprefix=$HOME/.local -Dinstall-helper=true
ninja -C builddir install
```

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `install-helper` | `false` | 同时安装 helper 和 CEF 运行时（约 1.5 GB） |
| `install-demo` | `true` | 安装 `cefnest-demo` 和回退用的测试客户端 |

发版流水线（`.cnb.yml`，只在版本 tag 上触发）打两个包，每个都有 deb 和 rpm 两种
格式：`cefnest` 包含库、helper 和 CEF 运行时；`cefnest-dev`（deb）/`cefnest-devel`
（rpm）额外提供头文件和 `cefnest.pc`。宿主程序用
`pkg-config --cflags --libs cefnest` 构建。

库不依赖环境变量也能找到 helper：先看 `CEFNEST_HELPER` 或 `CHROMIUM_HELPER`
（必须包含 `/`），再看编译时写入的 `prefix + libexecdir + <架构>` 路径，最后
回退到开发树。helper 进程本身会拿到哪些环境变量，见
`docs/control-channel.zh-CN.md`。

## 快速开始

运行 demo：

```bash
CHROMIUM_URL=https://example.com ./builddir/src/cefnest-demo
```

嵌入自己的应用——整个 API 就是三个对象（`examples/minimal.c` 是它的完整可运行版本）：

```c
GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

gtk_window_set_child (GTK_WINDOW (window), view);          // 1. view 放进窗口
chromium_session_set_view (session, CHROMIUM_VIEW (view)); // 2. view 交给 session
chromium_session_start (session, "https://example.com", &error); // 3. 启动
```

顺序很重要：必须先让 view 进入窗口，再启动 session，这样 helper 拿到的 compositor
才有真正可用的 surface。关闭窗口时调用 `chromium_session_stop()`（通常放在
`close-request` 处理器里）。

更多示例见 [`examples/`](examples/README.zh-CN.md)——每个任务一个专门的小程序：
工具栏、标签页、独立窗口、脚本、查找栏、下载、菜单、权限、错误处理、帧计数器、
User-Agent 切换、持久数据目录。每个示例演示什么，见
[examples/README.zh-CN.md](examples/README.zh-CN.md)。

## 数据目录

**不指定目录时，session 不向磁盘写任何东西。** helper 以 Chromium 的
incognito 模式运行：cookies、localStorage、IndexedDB 和 HTTP 缓存都在内存里，
CEF 必须写的那一个安装级目录是 `$XDG_RUNTIME_DIR` 下的 0700 临时目录，
`~/.cache` 下不会出现任何东西。要让页面数据跨重启保留，在 `start()` 之前
指定一个绝对路径：

```c
chromium_session_set_data_directory (session, "/home/me/.local/share/myapp/cefnest");
chromium_session_start (session, "https://example.com", &error);
```

完整契约——规则、目录放在哪里、各安装形态下卸载的行为、standalone helper
的环境变量、旧版 `~/.cache` 目录的清理——见
[docs/data-directory.zh-CN.md](docs/data-directory.zh-CN.md)。

## API

`ChromiumView` 是控件；`ChromiumSession` 是控制平面，负责启动 helper 并跟踪页面。
每个调用的完整约定都写在 `src/chromium-session.h` 的行内注释里；方法与信号的
分组速览见 [docs/api.zh-CN.md](docs/api.zh-CN.md)。

## demo 环境变量

| 变量 | 作用 |
| --- | --- |
| `CHROMIUM_URL` | 要打开的页面 |
| `CEFNEST_HELPER` | helper 路径（必须包含 `/`）；路径不存在时 demo 回退到测试客户端 |
| `CHROMIUM_HELPER` | 设为任意值即强制进入 session 模式；含 `/` 的值会作为 helper 路径原样使用，即使它不存在 |
| `CHROMIUM_SESSION_SCRIPT` | 脚本文件，一行一条命令（`load`、`js`、`find`、`menu-pick`、`download-accept`……） |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | N 毫秒后把控件截图写成 PNG 并退出 |
| `CHROMIUM_HELPER_NO_SANDBOX` | `1` 表示不带 Chromium 沙箱运行 helper |
| `CHROMIUM_USER_AGENT` | 在 helper 启动时写入的默认 User-Agent |
| `G_MESSAGES_DEBUG` | **空格分隔**的日志域：`all`，或 `nested chromium-view chromium-session` |

demo 还接受 `--data-dir=<绝对路径>`（见
[docs/data-directory.zh-CN.md](docs/data-directory.zh-CN.md)），并读取一些额外的开关
（`CEFNEST_DOWNLOAD_*`、`CEFNEST_PERMISSION`、
`CEFNEST_DRAG_OUT`、`CHROMIUM_WINDOW`、`CHROMIUM_FRAME_METRICS`、剪贴板 seed/watch
变量、`CHROMIUM_A11Y`），见 `src/main.c`。

## 常见问题

症状 → 处置速查表——沙箱被拒、helper 缺失、页面区域空白、日志域不输出、销毁
顺序——见 [docs/troubleshooting.zh-CN.md](docs/troubleshooting.zh-CN.md)。

## 已知限制

- 一个 seat、一个指针，不支持触摸；没有双指缩放，也不支持数位板输入。
- 在独立窗口中发起的拖动，会从错误的 surface 起始。
- `::popup-requested` 只携带 URL。
- 此 CEF 发行版不含 H.264/MP4；VP9 可以播放。
- 失败的导航会报告两次，而且 `::load-end` 仍会触发。
- 只有渲染进程的退出会被上报，且只上报它是怎么退出的。
- 下载状态到达 `complete` 并不等于文件已经出现。
- 查找一次只推进一个匹配项。
- helper 会扩展右键菜单；宿主无法添加自己的菜单项。
- 权限只能授予或拒绝，不能缩小范围；未应答的权限还会让页面失去下一次请求的机会。
- 页面音频绕过控件；`set_muted()` 是唯一的开关。
- 渲染进程运行在沙箱中；helper 本身没有沙箱，也不可能有。
- "查看网页源代码"显示的是**序列化后的 DOM**（包含脚本运行后的改动），
  不是网络返回的原始字节。
- API 尚不稳定——请链接到自己构建的版本。

## 文档

- `docs/data-directory.zh-CN.md` —— 数据目录契约：默认不落盘、一个路径持久化、
  互斥与卸载行为、旧版 `~/.cache` 目录的清理。
- `docs/api.zh-CN.md` —— 方法与信号速览；权威契约在 `src/chromium-session.h`。
- `docs/troubleshooting.zh-CN.md` —— 症状 → 处置，从 `No usable sandbox!` 到
  日志域。
- `docs/control-channel.zh-CN.md` —— session 与 helper 之间的 NDJSON 协议
  （命令、事件、四种交互形态）。只有自行编写 helper 或直接驱动通道时才需要。
- `site/` —— 项目主页（Vite + Vue + Bootstrap）；在目录里运行 `npm run dev`
  或 `npm run build`。

## 许可

LGPL-2.1-only——见 `LICENSE`。helper 链接了 CEF（BSD-3-Clause，附带其自身的第三方声明），
CEF 不由本仓库分发。
