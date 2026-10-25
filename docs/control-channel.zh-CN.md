# 控制通道协议

[English](control-channel.md) | 简体中文

`ChromiumSession` 与 helper 通过一条 unix socket 传输 NDJSON：宿主发送命令
（`{"cmd": …, …}`），helper 发送事件（`{"event": …, …}`），每行一个 JSON 对象。
`ChromiumSession` 的 API 和信号已经覆盖普通宿主的全部需求；只有自行编写 helper
或直接驱动通道时，才需要看这一页。

## helper 启动环境

宿主在启动子进程时会设置以下变量：

| 变量 | 值 |
| --- | --- |
| `WAYLAND_DISPLAY` | view 的嵌套 compositor socket 名称 |
| `GDK_BACKEND` | `wayland` |
| `XDG_SESSION_TYPE` | `wayland` |
| `CHROMIUM_HELPER_SOCKET` | 控制通道 socket 的完整路径 |
| `CHROMIUM_HELPER_DATA_DIR` | 持久 profile 的绝对路径；不存在即 incognito |

`DISPLAY` 和 `WAYLAND_SOCKET` 会被移除。`CHROMIUM_HELPER_DATA_DIR` 只在宿主
指定了数据目录时设置；否则即便宿主自己的环境里有这个变量，它也会被移除。
其余变量都由启动 helper 的一方负责。手工启动的 helper 读取同一个变量：
绝对路径即持久 profile，不设置即 incognito，使用 `$XDG_RUNTIME_DIR`（或
`/dev/shm`）下的私有目录。

## 命令（宿主 → helper）

| 命令 | 载荷 | 说明 |
| --- | --- | --- |
| `new-browser` | `url`、`mode`（`tab`/`window`，默认 `tab`） | 在同一 helper 中新建一个 CEF browser |
| `select-browser` | `browser` | 激活该标签页；窗口模式下还会聚焦窗口 |
| `close-browser` | `browser`（可选；默认当前激活） | 只关闭这一个；关闭最后一个会结束 helper |
| `load` | `url` | 作用于当前激活的 browser |
| `back` `forward` `reload` `stop` | `reload` 多一个 `hard` | 导航；作用于当前激活的 browser |
| `quit` | — | 关闭所有 browser 并结束 helper |
| `zoom-in` `zoom-out` `zoom-reset` | — | 步进 0.5；reset 回到 0.0 |
| `js` | `code`、`id` | 语句形式，在页面中求值（需要 CSP 允许 `unsafe-eval`） |
| `js-expr` | `code`、`id` | 表达式直接嵌入，不经过 `eval` |
| `mute` | `on`（省略时切换） | 静音 |
| `a11y` | `enabled` | `SetAccessibilityState(STATE_ENABLED/DEFAULT)` |
| `download-accept` | `id`、`path` | 绝对路径；父目录由宿主创建 |
| `download-reject` | `id` | 拒绝尚未开始的下载，或取消进行中的下载 |
| `download-pause` / `download-resume` | `id` | 只对已经拿到 item 回调的下载有效 |
| `find` | `pattern`、`forward`、`match_case`、`next` | 不返回 ack，结果以 `find-result` 事件送达 |
| `find-stop` | `clear`（默认为 true） | 结束查找并清除高亮 |
| `view-source` | — | 请求激活浏览器主 frame 的源码，以 `view-source-begin`/`-chunk`/`-end` 事件流回；菜单里的"查看网页源代码"走同一条路。无 ack——事件即应答 |
| `context-menu-select` | `id`、`flags`（可选） | 应答 `context-menu`；helper 一次只保留一个请求 |
| `context-menu-dismiss` | — | 关闭菜单，不选择任何项 |
| `permission-respond` | `id`、`result`（`accept`/`deny`/`dismiss`） | 应答 `permission-request`；media 的 `accept` 必须授予整个权限掩码 |

## 事件（helper → 宿主）

| 事件 | 载荷 |
| --- | --- |
| `ready` | `cef`、`pid`、`url`（握手） |
| `browser-created` | `browser`、`url`、`windowed` |
| `browser-selected` | `browser`、`windowed` |
| `browser-closed` | `browser` |
| `take-focus` | `browser`、`next`（页面的 Tab 遍历走出了文档边缘；宿主应把焦点链交给 view 之后——`next` 为 false 时是之前——的控件） |
| `view-source-begin` | `browser`、`id`、`url`、`title`、`bytes`（源码是序列化后的 DOM，含脚本改动，不是网络的原始字节） |
| `view-source-chunk` | `browser`、`id`、`data`（每块 64 KiB，完整 UTF-8 字符，有序） |
| `view-source-end` | `browser`、`id`、`truncated`（helper 上限 16 MiB；新的 `begin` 会放弃进行中的传输） |
| `view-source-error` | `browser`、`id`、`message`（可单独到来——frame 可能在流出任何内容前就已失效） |
| `address` / `title` | `url` / `title`（`address` 只在导航真正提交后发出） |
| `loading` | `loading`、`can_go_back`、`can_go_forward` |
| `load-end` | `url`、`http_code`（**失败时也会触发，此时 `http_code` 为 0**） |
| `load-error` | `error_code`、`url`、`message` |
| `renderer-exited` | `status`、`error_code`、`message`（只上报渲染进程的退出） |
| `muted` | `muted`（回读） |
| `zoom` | `level`（回读；每次主框架加载后也会触发一次——Chromium 按 origin 持久化缩放级别） |
| `copy-text` | `text`（菜单中的“复制链接/图片/媒体地址”） |
| `open-url` | `url`、`disposition`（菜单中的“在新窗口打开…”） |
| `popup-requested` | `url`、`disposition`、`gesture`、`browser` |
| `page-message` | `json`（页面调用 `cefnestPostMessage` 时传入的原始字符串） |
| `primary-selection` | `text`（页面选区；空字符串表示清除选区） |
| `js-result` | `id`、`ok`、`value`、`message` |
| `download-begin` | `id`、`url`、`suggested_name`、`mime_type`、`total_bytes` |
| `download-progress` | `id`、`received_bytes`、`total_bytes`、`percent`、`speed` |
| `download-end` | `id`、`state`（`complete`/`cancelled`/`interrupted`）、`path`、`received_bytes` |
| `find-result` | `identifier`、`count`、`active_ordinal`、`final` |
| `context-menu` | `x`、`y`、`items`（每项包含 `id`、`label`、`type`、`enabled`、`checked`） |
| `context-menu-dismissed` | 无（只在 helper 仍持有回调时发出） |
| `permission-request` | `id`、`kind`（`media`/`prompt`）、`origin`、`permissions[]`、`raw` |
| `permission-dismissed` | `id`、`result`（仅 prompt 有；media 没有 dismiss 回调） |
| `status` | `message` |
| `closed` | — |

凡是携带页面信息的事件（helper → 宿主）都会带 `browser` 字段。只有
`select-browser` 和 `close-browser` 会读取该字段；其余命令都作用于当前激活的 browser。

## 四种交互形态

1. **即发即忘**：导航、缩放、静音——发出即完成。
2. **有回执**：`js`/`js-expr` 携带调用方的 `id`，由 `js-result` 应答。注意 `find`
   故意不发送 ack——它的结果全部以 `find-result` 事件送达。
3. **挂起等待应答**：`download-begin` 会把 CEF 的回调挂起在 helper 里然后返回，宿主
   稍后用 `download-accept` / `download-reject` 应答。不应答就不会写入任何文件。
4. **必须应答**：`context-menu` 和 `permission-request`。沉默并非中立——菜单不应答，
   之后的所有右键都会被阻塞；权限不应答，页面的 promise 会一直处于 pending，媒体
   队列也会堵在它后面。

## 通道规则

- 每行一个 JSON 对象。宿主读取端把单行上限设为 **4 MiB**，超过就断开连接；包含
  NUL 字节的行会被整行丢弃。
- 缺失的字段会被折算成该类型的空值（字符串 → `""`，数字和布尔值 → `0`/`FALSE`，
  字节数 → `-1`）——不警告，也不拒收。helper 有权省略字段。
- 字节数以 JSON double 形式传输（2^53 以下精确；`CefDictionaryValue` 只有 32 位
  setter）。
- `download-*` 的 `id` 是 Chromium 的下载编号，会跨会话递增；`find-result` 的
  `identifier` 每次请求递增 1，只用于把同一次请求的多条回复归在一起。
- `total_bytes`/`percent` 为 `-1` 表示服务器未给出长度；`download-end` 的 `path`
  为空表示下载在开始前就被拒绝。
