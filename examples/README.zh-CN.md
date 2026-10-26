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
| `host.c` | 宿主必须应答的四个信号（下载、权限、JS、右键菜单——菜单故意不接）；通过 `CEFNEST_HOST_SCRIPT` 脚本驱动，可在无键盘下运行 |
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
| `devtools.c` | 开发者工具开关：F12 / 「检查」行，以及为每个开发者工具实例打开的 GTK 窗口 |
| `useragent.c` | 在运行时切换 User-Agent：重启 session，新的 `CHROMIUM_USER_AGENT` 会随 helper 一起生效 |
| `persistent.c` | 数据目录：localStorage 跨重启读回，以及用户要求遗忘时的 `clear_data()` |
| `plugin-adblock/` | Web 前端的插件（不是宿主）：EasyList 形状的外观过滤、按站点开关、Alt+Shift+单击拾取元素、`chrome.storage`、会查 `chrome.tabs` 的弹窗——以及它跨不过去的那条线（没有 `webRequest`） |

每个文件的头注释就是它的完整说明；先读 `minimal.c`，再按需取用。

这个插件不需要单独的构建步骤。把它装进插件的安装根目录
`<data-dir>/cefnest-plugins/`，再打开演示页；同一个数据目录也是规则和计数
跨次运行保留的原因：

```bash
DATA=$(mktemp -d)
mkdir -p "$DATA/cefnest-plugins"
cp -r examples/plugin-adblock "$DATA/cefnest-plugins/"
CHROMIUM_HELPER=$PWD/helper/build/Release/cefnest-helper \
  ./builddir/src/cefnest-demo --data-dir="$DATA" \
  file://$PWD/examples/plugin-adblock/demo.html
```

不带 `--data-dir` 也能运行，但规则只活到进程退出。（`--plugin-dir=$PWD/examples`
也可以，不过它会把原生示例包一起扫进来——那些包的 `.so` 在 `builddir` 里，
日志里会出现 `library-open-failed`。）

演示页把内容脚本藏掉了多少写在标题里——`AD-SIFT:<hidden>/<total>:style:custom`，
后两段表示插件的两个 style 元素是否在页面上——底部状态条也会显示。内置列表在
`document_start` 生效，早于页面自己的内联脚本；手工拾取的规则要从
`chrome.storage` 读回，会晚几毫秒才落地。
