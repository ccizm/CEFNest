# examples/

[English](README.md) | 简体中文

每个示例只演示一件事，本身就是一个完整、可直接运行的宿主。构建后它们位于
`builddir/examples/cefnest-<名称>`；不会随 `meson install` 安装。

```bash
ninja -C builddir
CEFNEST_URL=https://example.com ./builddir/examples/cefnest-tabs
```

helper 的查找方式见仓库根目录 `README.zh-CN.md` 的**安装**一节；session 与 helper
之间的通道协议见 [`docs/control-channel.zh-CN.md`](../docs/control-channel.zh-CN.md)。

| 示例 | 演示什么 |
| --- | --- |
| `minimal.c` | 最小宿主：窗口 + `ChromiumView` + `ChromiumSession`，约 56 行 |
| `host.c` | 页面需要等宿主应答的四个信号（下载、权限、JS、`::browser-created`）；通过 `CEFNEST_HOST_SCRIPT` 脚本驱动，可无头运行 |
| `browser.c` | 单页浏览器外壳：地址栏、前进后退、缩放、静音——全部依赖 `::changed` 和各个 getter，无需逐事件维护状态 |
| `tabs.c` | 多标签页：多个 browser 共用一个 helper，以及 `::browser-created` / `::browser-selected` 如何驱动标签栏 |
| `windows.c` | 窗口模式的 browser：`chromium_view_new_shared()` 为每个 CefWindow 提供一个真正的 GTK 窗口 |
| `script.c` | 两种执行 JavaScript 的方式与 `::script-result`；`cefnestPostMessage()` / `::page-message` 的页面 ↔ 宿主往返 |
| `findbar.c` | 页内查找：`find()` 与 `::find-result` 的增量结果如何构成一个查找栏 |
| `downloads.c` | 下载：`::download-begin` 必须应答，每个下载一行进度，`::download-end` 的三种结束状态 |
| `menus.c` | 右键菜单：自行绘制 popover，并在返回 TRUE 后应答；不接管时使用 session 的默认菜单 |
| `permissions.c` | 权限提示：把 `::permission-request` 呈现为 allow/deny 行，以及不应答的代价 |
| `errors.c` | 三种“页面已不是原来的样子”的信号：`::load-error`、`::renderer-exited`、`::closed`，以及各自如何接入 |
| `observe.c` | 帧计数器与无障碍开关：让“页面卡住了”变得可测量 |
| `useragent.c` | 在运行时切换 User-Agent：重启 session，新的 `CHROMIUM_USER_AGENT` 会随 helper 一起生效 |

每个文件的头注释就是它的完整说明；先读 `minimal.c`，再按需取用。
