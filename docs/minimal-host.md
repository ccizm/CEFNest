# 最小宿主：三个对象，四十行

`examples/minimal.c` 是一个真正能运行的宿主：打开窗口、让页面填满窗口、关窗时停掉 helper。
它不包含 demo 的地址栏、下载面板、查找栏或测试脚本，目的是让人先看见最小接线。

## 1. 编译与运行

```bash
meson setup builddir --wipe
ninja -C builddir
./builddir/examples/cefnest-minimal
```

helper 按 `docs/packaging.md` 的规则查找：`CEFNEST_HELPER` / `CHROMIUM_HELPER` /
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
| 地址、标题、加载状态 | `chromium_session_get_*`，`::changed`、`::load-end` |
| 前进/后退/刷新 | `chromium_session_go_*`、`reload`、`stop_loading` |
| 页面 JS | `chromium_session_run_javascript()` / `run_javascript_expression()`；答案在 `::script-result` |
| 下载 | `::download-begin` + `accept_download()` / `reject_download()` |
| 权限 | `::permission-request` + `accept/deny/dismiss` |
| 右键菜单 | `::context-menu`；不接时 session 自己画 popover |
| 页面消息 | `cefnestPostMessage()` → `::page-message` |
| 主选区 | 页面选中文字 → `::primary-selection`（session 自动写 Gtk primary） |
| 读屏器 | 自动监听 `org.a11y.Status.ScreenReaderEnabled`；也可显式 `chromium_session_set_accessibility()` |

完整约定见 `src/chromium-session.h`、`src/chromium-view.h` 和 `docs/control-channel.md`。
