# 最小宿主：三个对象，五十几行

`examples/minimal.c` 是一个真正能运行的宿主：打开窗口、让页面填满窗口、关窗时停掉 helper。
它不包含 demo 的地址栏、下载面板、查找栏或测试脚本，目的是让人先看见最小接线。
文件实际 56 行。

## 1. 编译与运行

```bash
meson setup builddir --wipe
ninja -C builddir
./builddir/examples/cefnest-minimal
```

helper 按 `docs/guides/packaging.md` 的规则查找：`CEFNEST_HELPER` / `CHROMIUM_HELPER` /
编译进去的安装路径 / 开发树。安装后的宿主不需要环境变量。

起始页可以用 `CEFNEST_URL` 覆盖：

```bash
CEFNEST_URL=https://developer.mozilla.org/ ./builddir/examples/cefnest-minimal
```

## 2. 三个对象

| 对象 | 作用 |
| --- | --- |
| `GtkWindow` | 普通 GTK 窗口；`ChromiumView` 是它的 child |
| `ChromiumView` | 自建嵌套 Wayland compositor + 一个 `GdkSurface`；页面画在它里面 |
| `ChromiumSession` | 起 helper、监听控制 socket、管理导航/标题/下载/菜单等状态 |

顺序不能反：先把 view 放进 window，再 `chromium_session_set_view()`，最后
`chromium_session_start()`。helper 的 `WAYLAND_DISPLAY` 来自 view 的 compositor socket；
view 没挂到窗口上时 compositor 还没有可用的 surface。

## 3. 最小代码

```c
GtkWidget *window = gtk_application_window_new (app);
ChromiumView *view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

chromium_session_set_view (session, view);
gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
gtk_window_present (GTK_WINDOW (window));
chromium_session_start (session, "https://example.com", &error);

g_object_set_data_full (G_OBJECT (window), "session", session, g_object_unref);
```

关窗时调用 `chromium_session_stop()`；session 的 `dispose` 会收掉 helper 和 socket。示例用
一个 `close-request` 处理器做这件事。

## 4. 接下来接什么

最小宿主只连接了页面和生命周期。库其余公开面按需要加：

| 需求 | API / 信号 |
| --- | --- |
| 地址、标题、加载状态、历史可用性 | `chromium_session_get_uri()`、`chromium_session_get_title()`、`chromium_session_get_loading()`、`chromium_session_get_can_go_back()`、`chromium_session_get_can_go_forward()`、`chromium_session_get_running()`；变化走 `::changed`，加载完走 `::load-end`（失败也发，HTTP code 0） |
| 前进/后退/刷新 | `chromium_session_go_back()`、`chromium_session_go_forward()`、`chromium_session_reload()`、`chromium_session_stop_loading()`、`chromium_session_load_uri()` |
| 页面 JS | `chromium_session_run_javascript()`（语句）/ `chromium_session_run_javascript_expression()`（表达式，不经过 `eval`）；答案在 `::script-result`（id, ok, value, message） |
| 下载 | `::download-begin` + `chromium_session_accept_download()` / `chromium_session_reject_download()`；在飞的还能 `chromium_session_pause_download()` / `chromium_session_resume_download()` |
| 权限 | `::permission-request` + `chromium_session_permission_respond()`（`ACCEPT`/`DENY`/`DISMISS`）；被页面撤回走 `::permission-dismissed` |
| 右键菜单 | `::context-menu` + `chromium_session_context_menu_select()` / `chromium_session_context_menu_dismiss()`；不接时 session 自己画 popover |
| 页面消息 | 页面调 `cefnestPostMessage()` → `::page-message`；回去用 `chromium_session_run_javascript()` 派发 `MessageEvent` |
| 查找 | `chromium_session_find()` / `chromium_session_stop_finding()`；答案在 `::find-result` |
| 静音 | `chromium_session_set_muted()` / `chromium_session_get_muted()` |
| 缩放 | `chromium_session_zoom()`（`in`/`out`/`reset`）/ `chromium_session_get_zoom_level()`；落地走 `::zoom-changed` |
| 多浏览器 | `chromium_session_new_browser()` / `chromium_session_select_browser()` / `chromium_session_close_browser()`；`::browser-created` / `::browser-selected` / `::browser-closed`（tab 关不掉，见 §5） |
| 弹窗 | `::popup-requested`（只有 url） |
| 主选区 | 页面选中文字 → `::primary-selection`（session 自动写 Gtk primary） |
| 读屏器 | 自动监听 `org.a11y.Status.ScreenReaderEnabled`；也可显式 `chromium_session_set_accessibility()` |
| 帧计数 | `chromium_session_get_frame_count()` |

`chromium_view_get_compositor()` / `chromium_view_get_socket_name()` 在
`src/chromium-view.h`——要自己起第二个 view（`chromium_view_new_shared()` +
`set_target_surface()`，`docs/features/multi-browser.md` §2）时用。

完整约定见 `src/chromium-session.h`、`src/chromium-view.h` 和 `docs/guides/control-channel.md`。

## 5. 中等示例 `examples/host.c`：把"问题"信号接完

§4 那张表里只有四行是**问答**：下载、权限、右键菜单、页面 JS。`examples/host.c` 把
其中三个接成能点的行（菜单故意不接，好让"什么都不写也自带的 popover"留在画面里对照），
外加一个 helper 里开多个浏览器的标签行。它是 56 行之后的下一步，不是 demo——demo 有
查找栏、拖出、静音、缩放和一个 2000 行的脚本解释器。

不带键盘跑（`test/ci-session.sh` 起一个没人看得见的嵌套 GNOME Shell）：

```bash
printf 'load http://gate.test/download?name=report.pdf&bytes=4096\nwait 800\nsave /tmp/dl\nwait 1600\nquit\n' > /tmp/steps
CEFNEST_HOST_SCRIPT=/tmp/steps CEFNEST_HELPER=$PWD/test/fake-helper.py \
  ./test/ci-session.sh ./builddir/examples/cefnest-host http://gate.test/start
```

动词一个一行：`load tab select close js save drop allow deny wait quit`。每步都同时进
 stderr 和窗口里的日志页，所以无头跑断言的是宿主**做了**什么，不是它打算做什么。

### 实测（2026-10-22）

fake helper 这一侧，逐字：

```
host: download-begin 1 report.pdf from http://gate.test/download?name=report.pdf&bytes=4096
host: download 1 -> /tmp/dl/report.pdf
host: download-progress 1 1024 of 4096 B, 25 %, 20480 B/s
host: download-end 1 state=complete path=/tmp/dl/report.pdf bytes=4096
```
落盘 4096 字节。拒绝那条路：

```
host: download 3 refused
host: download-end 3 state=cancelled path=(none) bytes=0
```
`ls /tmp/host-dl` 里没有 `refused.bin`——**没答或答"不要"都不写文件**，这是
`docs/features/download-path.md` 的模型，示例这边只是不复述它。

名字是页面给的，所以清洗是宿主的活，这一段是专门为它跑的：`name=../escape.pdf` 落成
`/tmp/host-dl/escape.pdf`，`/tmp/escape.pdf` 不存在；目录里已经有 `report.pdf` 和
`report (1).pdf` 时，第三个同名下载拿到 `report (2).pdf`。

权限一问一答，两边都留了字：

```
host: permission-request 1 kind=media raw=3 [device-audio, device-video]
host: permission 1 answered accept
fake: permission-respond id=1 result=accept kind=media names=device-audio+device-video
```

真 helper 这一侧（`helper/build/Release/cefnest-helper`），标签和 JS 回读：

```
host: browser-selected 1
host: browser-created 1 windowed=0
host: browser-created 2 windowed=0 file:///…/test/dnd-page.html?second
host: browser-selected 2
host: script 1 = "DND-PAGE"
```
注意头两行的**顺序**：第一次 `::browser-selected` 抢在 `::browser-created` 前面到了，
所以"哪个标签是活的"必须记下来、等按钮出现时再补，不能只在信号里刷一次样式。

### 关不掉标签（实测到的缺口，不是示例的写法问题）

同一个 helper、同一个 id 换三种问法：

- `close-browser 99` → `session: helper status: no-such-browser`：命令确实分发到了；
- `close-browser 2`（tab）→ 之后什么都没有。没有 `helper: browser 2 closing`，
  没有 `browser-closed`，等了 4 秒；
- `close-browser 3`（windowed）→ `helper: browser 3 closing`、`browser 3 closed`、
  `browser 1 selected`。

也就是说 `close_browser()` 只对独立窗口有效，标签是静默空操作，宿主这侧无从补救
（`::browser-closed` 收不到，就不能拆自己的 tab）。记在 `code-review-2026-10-22.md`
的 M22。示例里标签上的 ✕ 和 `close` 动词都保留，注释写明它今天关不掉什么。

### 窗口先走，session 后走

`gtk_window_destroy()` 之后 widget 全没了，而 session 还会再回一次——`::closed` 必然在
`chromium_session_stop()` 里同步发出来。第一版没管这个，退出时刷了一串
`invalid unclassed pointer in cast to 'GtkBox'` / `'GtkTextView'` 和
`gtk_box_remove: assertion 'GTK_IS_BOX (box)' failed`。现在窗口的 `destroy` 处理器把
`Host` 里每个 widget 指针置 NULL、把待答行只当内存释放（不碰已经要死的 widget），
每个会画界面的处理器开头读这一个标志：下载问题落在窗口之后**不答**（不答就不写文件），
权限问题落在之后返回 FALSE（session 依约拒绝）。修完之后同一条 `quit` 路径零告警。

