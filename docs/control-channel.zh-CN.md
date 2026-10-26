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
| `CHROMIUM_HELPER_AUTOPLAY_POLICY` | `--autoplay-policy` 的值（如 `user-gesture-required`）；不设置即 Chromium 默认。同样是 spawn 期一次性：无值时被移除，继承不来 |

`DISPLAY` 和 `WAYLAND_SOCKET` 会被移除。`CHROMIUM_HELPER_DATA_DIR` 只在宿主
指定了数据目录时设置；否则即便宿主自己的环境里有这个变量，它也会被移除。
其余变量都由启动 helper 的一方负责。手工启动的 helper 读取同一个变量：
绝对路径即持久 profile，不设置即 incognito，使用 `$XDG_RUNTIME_DIR`（或
`/dev/shm`）下的私有目录。

## 命令（宿主 → helper）

| 命令 | 载荷 | 说明 |
| --- | --- | --- |
| `new-browser` | `url`、`mode`（`tab`/`window`/`background`，默认 `tab`） | 在同一 helper 中新建一个 CEF browser；`background` 跑一个没有窗口、没有 surface 的页面（插件清单的 `background.service_worker` 骑在这上面） |
| `select-browser` | `browser` | 激活该标签页；窗口模式下还会聚焦窗口 |
| `close-browser` | `browser`（可选；默认当前激活） | 只关闭这一个；关闭最后一个会结束 helper |
| `load` | `url` | 作用于当前激活的 browser |
| `back` `forward` `reload` `stop` | `reload` 多一个 `hard`；五个命令都接受可选 `browser`（缺省为激活） | 导航，可点名后台标签 |
| `quit` | — | 关闭所有 browser 并结束 helper |
| `zoom-in` `zoom-out` `zoom-reset` | — | 步进 0.5；reset 回到 0.0 |
| `js` | `code`、`id` | 语句形式，在页面中求值（需要 CSP 允许 `unsafe-eval`） |
| `js-expr` | `code`、`id` | 表达式直接嵌入，不经过 `eval` |
| `mute` | `browser`（可选）、`on`（省略时切换） | 静音；`browser` 指名标签页（`tabs.update` 的 muted 路径），`muted` 事件回报名下真正被静音的 browser |
| `a11y` | `enabled` | `SetAccessibilityState(STATE_ENABLED/DEFAULT)` |
| `devtools` | `enabled` | 提供右键菜单的「检查」与 F12 / Ctrl+Shift+I；开发者工具 browser 以带 `devtools` 的 `browser-created` 到达 |
| `download-accept` | `id`、`path` | 绝对路径；父目录由宿主创建 |
| `download-reject` | `id` | 拒绝尚未开始的下载，或取消进行中的下载 |
| `download-pause` / `download-resume` | `id` | 只对已经拿到 item 回调的下载有效 |
| `find` | `pattern`、`forward`、`match_case`、`next`、可选 `browser` | 不返回 ack，结果以 `find-result` 事件送达 |
| `find-stop` | `clear`（默认为 true）、可选 `browser` | 结束查找并清除高亮 |
| `view-source` | 可选 `browser` | 请求该浏览器主 frame 的源码，以 `view-source-begin`/`-chunk`/`-end` 事件流回；菜单里的"查看网页源代码"走同一条路。无 ack——事件即应答 |
| `history-go` | `index`、可选 `browser` | CEF 公开 API 没有的那一个历史跳转：helper 用自己的 DevTools 管道跑 `Page.getNavigationHistory` + `Page.navigateToHistoryEntry`——不要求宿主 `cdp-enable`，这是一次有范围的能力。跳转以常规 `address`/`load-*` 事件呈现；无独立回执 |
| `zoom-set` | `level`、可选 `browser` | 一个显式 CEF zoom level（0.0 = 100%）；zoom 家族与 `stop` 同样接受可选 `browser` |
| `ua-set` | `value`、可选 `browser` | 用户代理。不带 `browser` 是 session 级属性（现有每个 browser 都会收到，新建的在创建时继承）；带则只覆盖该 browser。空值 = 回到 Chromium 自身（经 `Browser.getVersion` 捕获）。每次加载开始都会重放，渲染进程崩溃不会丢 |
| `context-menu-select` | `id`、`flags`（可选） | 应答 `context-menu`；helper 一次只保留一个请求 |
| `context-menu-dismiss` | — | 关闭菜单，不选择任何项 |
| `permission-respond` | `id`、`result`（`accept`/`deny`/`dismiss`） | 应答 `permission-request`；media 的 `accept` 必须授予整个权限掩码 |
| `js-dialog-respond` | `id`、`ok`、`value` | 应答 `js-dialog`；`value` 是 prompt 的输入文本，其他类型忽略它 |
| `file-dialog-respond` | `id`、`paths[]` | 应答 `file-dialog`；空列表即取消 |
| `certificate-respond` | `id`、`allow` | 应答 `certificate-error`；默认（宿主未连接）是拒绝 |
| `certificate-authorize` | `fingerprint` | 该证书在本 helper 生命周期内免询问放行；只存内存，跨重启的持久化归宿主自己的策略存储 |
| `certificate-clear-authorized` | — | 撤销全部指纹豁免。两张表一起清：helper 自己的指纹表，加上 Chromium 在 `OnCertificateError` 继续（Continue）之后按 (host, 证书) 自己记住的那份。CEF 还建议同时 `CloseAllConnections`——本 CEF 实测该调用在浏览器活着时会段错误，而不清连接重新连接照样会再询问，所以只清例外表 |
| `auth-respond` | `id`、`user`、`password` | 应答 `auth-required`：请求以这组凭据继续 |
| `auth-cancel` | `id` | 取消 `auth-required`——请求以 401 失败，即 Chrome 自己的默认 |
| `client-cert-respond` | `id`、`index` | 应答 `client-cert-request`；-1 表示不选证书继续握手，也是默认值 |
| `render-unresponsive-respond` | `id`、`action`（`wait`/`terminate`） | 应答 `render-unresponsive`：`wait` 重置 CEF 的计时继续等，`terminate` 杀掉渲染进程（随后照常来 `renderer-exited`）。不应答 = CEF 默认，静默继续等——杀进程的决定不能由超时代答 |
| `tls-info` | `id`（宿主自己的）、可选 `browser` | 读一次已加载页面的证书事实，走组件自己的管道（`Security.enable` + `visibleSecurityStateChanged`；CEF 156 没有 `getVisibleSecurityState`）。由 `tls-info` 应答；五秒无人应答的提问以 `ok` 假收场 |
| `capture-screenshot` | `id`（宿主自己的）、可选 `browser` | 页面的一张 PNG（`Page.captureScreenshot`），由 `screenshot-result` 应答；超约 3 MiB 以 `screenshot-too-large` 拒发而非发出不可读的行 |
| `clear-data` | `types`（位掩码：1 缓存，2 cookies，4 HTTP 认证缓存，8 源存储）、`origins[]`（仅源存储需要） | 同一条管道上的分类型清除（`Network.clearBrowserCache` / `clearBrowserCookies` / `clearHttpAuthCache`，源存储逐 origin 跑 `Storage.clearDataForOrigin`）。即发即忘；每次调用自己记录结果 |
| `script-add` | `id`、`patterns[]`、`code`、`run_at`（`document_start`/`document_end`/`document_idle`，默认 idle）、`all_frames`、可选 `browser`（缺省全部浏览器） | 登记一条宿主脚本；同 id 再发是整体替换。`document_start` 在渲染进程里、先于页面内联脚本注入；其余两个搭主框架加载结束的车。没有隔离世界——脚本就在页面自己的 JavaScript 域里跑 |
| `script-remove` | `id` | 摘掉这条脚本 |
| `scheme-register` | `name`、`flags`（可选，只对下一次 start 生效） | 立即挂上该 scheme 的宿主数据工厂；**选项**只在进程启动时从 `CHROMIUM_HELPER_SCHEMES` 读，此刻带来的 `flags` 只记日志说明已延后。名字必须匹配 `[a-z][a-z0-9+.-]*`；内建 scheme 与 `cefnest-extension` 一律拒收 |
| `scheme-unregister` | `name` | 摘掉该工厂 |
| `scheme-response` | `id`、`status`、`reason`、`content_type`、`headers`（可选 JSON 对象） | 应答 `scheme-request`：报出响应，打开 body |
| `scheme-chunk` | `id`、`data`（base64，每片 48 KiB 原始字节） | 一段 body |
| `scheme-end` | `id` | 收口 body |
| `scheme-abort` | `id` | 宿主收回该请求；加载随之取消 |
| `intercept-add` | `id`、`patterns[]`（match pattern）、`resource_types[]`（可选：`main_frame`、`image`、`xhr`…） | 在宿主自己的 id 下登记一条拦截规则；同 id 再发是整体替换。无规则注册 = 功能关闭、零开销 |
| `intercept-remove` | `id` | 摘掉这条规则 |
| `intercept-mode` | `passive` | passive 只上报不阻塞；默认 blocking |
| `request-respond` | `request_id`、`action`（`allow`/`cancel`/`redirect`）、`url`（仅 redirect） | 应答 `request-intercept`；redirect 先换 URL。无超时——页面在等宿主 |
| `filter-add` | `id`、`patterns[]`（match pattern）、`ops[]`（按序：`{type: "replace", find, replace}` 各 ≤ 64 KiB、`{type: "inject-css", css}` ≤ 1 MiB、≤ 32 条 op） | 登记一条声明式响应过滤规则：helper 自己的引擎在匹配响应的字节流上改写——replace 做跨块拼接（块边界上的匹配也命中），inject-css 只对 `text/html` 在 `</head>` 前注入（流里没有该标记就附加在末尾）。任何问题都不在通道上挂起；无规则注册 = 什么都不存在。`drop-header` op 被点名拒绝（`filter-add-drop-header-unsupported`）：CEF 156 没有剔除响应头的面，`OnResourceResponse` 与 `GetResourceResponseFilter` 两处实测（2026-10-26）改写均不上线 |
| `filter-remove` | `id` | 摘掉这条过滤规则；已经流出中的响应用它出厂时的 ops 跑完 |
| `cdp-enable` | `browser`（可选） | 在该 browser 上注册 DevTools 协议观察者；这是放行 `cdp-execute` 的唯一开关 |
| `cdp-disable` | `browser`（可选） | 注销观察者 |
| `cdp-execute` | `id`（宿主自己的）、`method`、`params`（JSON 文本） | 一条 DevTools 方法调用；观察者未开时以 `cdp-not-enabled` 拒绝。由 `cdp-result` 应答 |
| `plugins-load` | `plugins[]`：`id`、`root`、`background`（可选，`background.service_worker` 路径）、`scripts[]`（`matches`、`js`、`run_at`） | 一个已启用插件的全部事实：把 root 注册到 `cefnest-extension://`（factory 只服务这些目录），存下它的内容脚本，并指明合成的 `__cefnest_background__.html` 页面要跑哪个脚本。对同一个 id 再发一次是整体替换——helper 不积累历史 |
| `plugins-unload` | `id` | 摘掉这个插件的 root 与脚本表 |
| `menu-add-user-item` | `slot`（0–199）、`label`、`when`（可选 match pattern） | 在 26600+slot 处加一条右键菜单行，构建菜单模型时追加在末尾；`when` 不匹配的页面不显示该行 |
| `menu-remove-user-item` | `slot` | 摘掉这条行（disable/卸载时） |
| `plugin-respond` | `browser`、`reply_id`、`ok`、`value`（JSON 文本） | 宿主对一条 `plugin-message` 的应答；路由到那个浏览器的渲染进程，由持有该 reply id 的 shim 兑现。`value` 超 1 MiB 以 `plugin-payload-too-large` 拒收 |
| `browsers-list` | `id`（请求 id） | 请求浏览器表；由同名 `browsers` 事件带着同一 `id` 应答 |

## 事件（helper → 宿主）

| 事件 | 载荷 |
| --- | --- |
| `ready` | `version`（本 helper 说话的控制通道契约；大版本与 session 不符的 helper 会被就地停掉）、`cef`、`pid`、`url`（握手） |
| `browser-created` | `browser`、`url`、`windowed`、`devtools`、`background`（CEF 为 `ShowDevTools` 创建的开发者工具 browser：始终是 windowed，窗口归 CEF 所有，永远不会成为激活 browser；`background` 为真标记常驻隐藏页——无 surface、永不激活、不进 `browsers` 表） |
| `browser-selected` | `browser`、`windowed` |
| `browser-closed` | `browser` |
| `take-focus` | `browser`、`next`（页面的 Tab 遍历走出了文档边缘；宿主应把焦点链交给 view 之后——`next` 为 false 时是之前——的控件） |
| `view-source-begin` | `browser`、`id`、`url`、`title`、`bytes`（源码是序列化后的 DOM，含脚本改动，不是网络的原始字节） |
| `view-source-chunk` | `browser`、`id`、`data`（每块 64 KiB，完整 UTF-8 字符，有序） |
| `view-source-end` | `browser`、`id`、`truncated`（helper 上限 16 MiB；新的 `begin` 会放弃进行中的传输） |
| `view-source-error` | `browser`、`id`、`message`（可单独到来——frame 可能在流出任何内容前就已失效） |
| `address` / `title` | `url` / `title`（`address` 只在导航真正提交后发出） |
| `loading` | `loading`、`can_go_back`、`can_go_forward` |
| `load-started` | `browser`、`url`、`transition`（Chromium 位掩码）、`is_main_frame`——每次完成提交的导航一条；提交前就失败的导航到不了这里 |
| `load-end` | `browser`、`url`、`http_code`（**失败时也会触发，此时 `http_code` 为 0**）——每次导航恰一条 |
| `load-error` | `browser`、`error_code`、`url`、`message`——每次失败导航恰一条：Chromium 晚几秒跟进的 ERR_ABORTED 回声由 helper 丢弃 |
| `renderer-exited` | `status`、`error_code`、`message`（只上报渲染进程的退出） |
| `muted` | `browser`、`muted`（回读；`browser` 名下真正被静音的那个——后台标签页安静了不代表当前视图变了） |
| `zoom` | `level`（回读；每次主框架加载后也会触发一次——Chromium 按 origin 持久化缩放级别） |
| `copy-text` | `text`（菜单中的“复制链接/图片/媒体地址”） |
| `open-url` | `url`、`disposition`（菜单中的“在新窗口打开…”） |
| `popup-requested` | `url`、`disposition`、`gesture`、`browser` |
| `page-message` | `json`（页面调用 `cefnestPostMessage` 时传入的原始字符串） |
| `primary-selection` | `text`（页面选区；空字符串表示清除选区） |
| `js-result` | `id`、`ok`、`value`、`message`（`ok` 为真当且仅当脚本运行**且**结果送达；超过 helper 64 KiB 上限的结果以 `ok` 假、`value` null、`message` 说明丢弃的方式送达，不再是"成功但恰好没有值"） |
| `download-begin` | `id`、`url`、`suggested_name`、`mime_type`、`total_bytes` |
| `download-progress` | `id`、`received_bytes`、`total_bytes`、`percent`、`speed` |
| `download-end` | `id`、`state`（`complete`/`cancelled`/`interrupted`）、`path`、`received_bytes` |
| `find-result` | `identifier`、`count`、`active_ordinal`、`final` |
| `context-menu` | `x`、`y`、`items`（每项包含 `id`、`label`、`type`、`enabled`、`checked`） |
| `context-menu-dismissed` | 无（只在 helper 仍持有回调时发出） |
| `permission-request` | `id`、`kind`（`media`/`prompt`）、`origin`、`permissions[]`、`raw` |
| `permission-dismissed` | `id`、`result`（仅 prompt 有；media 没有 dismiss 回调） |
| `js-dialog` | `id`、`kind`（`alert`/`confirm`/`prompt`/`beforeunload`）、`origin`、`message`、`default_value` |
| `js-dialog-closed` | `id`（browser 收回了这边持有的对话框：导航重置或关闭） |
| `file-dialog` | `id`、`mode`（`open`/`open-multiple`/`open-folder`/`save`）、`title`、`default_path`、`accept[]` |
| `file-dialog-closed` | `id`（选择器还挂着时 browser 关闭了） |
| `certificate-error` | `id`、`error`（Chromium 的 net-error 名）、`url`、`subject`、`issuer`、`fingerprint`（叶子证书 DER 的 SHA-256，十六进制）——导航停住等待应答；默认拒绝 |
| `certificate-error-closed` | `id`（问题还挂着时 browser 导航或关闭了） |
| `auth-required` | `id`、`browser`、`is_proxy`、`scheme`（`basic`/`digest`…）、`realm`、`origin`——请求停住等待 `auth-respond`/`auth-cancel`；无人接听时 session 取消，请求以 401 失败 |
| `auth-closed` | `id`、`browser`（问题还挂着时 browser 导航或关闭了） |
| `client-cert-request` | `id`、`browser`、`origin`（`host:port`）、`certs[]`（`subject`、`issuer`、`fingerprint`）——握手停住等待 `client-cert-respond`；默认不选 |
| `client-cert-closed` | `id`、`browser` |
| `render-unresponsive` | `id`、`browser`——渲染进程 15 秒以上不处理输入事件（CEF 文档原话）；问题挂起等 `render-unresponsive-respond`，每 browser 至多一条，不应答 = 继续等。**实测（2026-10-26）本 CEF 的 Alloy/嵌套 Wayland 形态下该回调不触发**（JS 死循环 90 秒 + 向挂起页面点击，观察 35 秒无事件）；面保留，实测记录在此 |
| `render-responsive` | `id`、`browser`（挂着的页面自己缓过来了；宿主面板收起，之后再应答无效） |
| `render-unresponsive-closed` | `id`、`browser`（问题还挂着时 browser 关闭了） |
| `console` | `browser`、`level`（`debug`/`info`/`warning`/`error`）、`message`（1 MiB 截断）、`source`、`line`——页面 `console.*` 的逐条转发；没有它这些行只存在于 DevTools agent 里 |
| `tooltip-changed` | `browser`、`text`（空串 = 收起；helper 返回 true 压掉 CEF 自己的 tooltip 弹层，画法归宿主）——**实测（2026-10-26）本形态下 CEF 的 views 层从不调用 `OnTooltip`**，面保留 |
| `status-message` | `browser`、`text`（悬停链接时是目标 URL） |
| `focus-requested` | `browser`、`source`（`navigation`/`system`）——browser 即将取得焦点；只上报不拦 |
| `media-access-changed` | `browser`、`video`、`audio`——采集开始或停止；devtools 之外照发，**后台页也发**：隐藏页面占着麦克风时硬件灯是亮的，指示器不亮就是谎报 |
| `print-requested` | `browser`——页面调了 `window.print()`。从打印事务的起点上报（实测：`OnPrintStart` 必触发，而 `OnPrintDialog` 只在环境里有打印机时才会到）；组件不开打印对话框，宿主要打印就调 `print-pdf` 喂自己的 GTK 对话框 |
| `frame-created` | `browser`、`frame`（CEF 156 的字符串标识；"-4" 占位 frame 已滤掉）、`is_main`、`url`（created 时刻通常为空——URL 要等导航提交） |
| `frame-destroyed` | `browser`、`frame` |
| `frame-attached` | `browser`、`frame`、`reattached`（从 BackForwardCache 或连接错误中回来时为 true） |
| `frame-detached` | `browser`、`frame` |
| `main-frame-changed` | `browser`、`old_frame`、`new_frame`（首次赋值 old 为空串，最后移除 new 为空串；跨源导航换进程时宿主的归因靠它） |
| `tls-info` | `id`（提问的）、`browser`、`ok`、`security_state`（`secure`/`insecure`/`insecure-broken`…）、`subject`、`issuer`、`fingerprint`（叶证书 DER 的 SHA-256 十六进制；状态不带证书时为空串）、`valid_from`、`valid_to`（Unix 秒） |
| `screenshot-result` | `id`、`browser`、`ok`、`data`（base64 PNG；`ok` 时才有）、`message`（触顶时为 `screenshot-too-large`） |
| `window-properties` | `browser`、`x`、`y`、`w`、`h`、`toolbar`、`menubar`、`status`、`resizable`、`fullscreen`——窗口化浏览器的窗口在创建时与边界/全屏变化时上报；本 helper 的窗口无框，toolbar/menubar/status 恒为 false |
| `browsers-changed` | `browsers[]`（`id`、`url`、`title`、`active`、`windowed`、`muted`、`zoom`——整表，创建/关闭/选中/标题/地址/静音/缩放时重推） |
| `scheme-request` | `id`、`browser`、`url`、`method`、`headers`（一个 JSON 对象）——加载停住等待 `scheme-response`/`scheme-abort`；无人接听时 session 中止，因为组件不编造页面数据 |
| `scheme-closed` | `id`、`browser`（请求中途 browser 没了） |
| `request-intercept` | `request_id`、`url`、`method`、`resource_type`、`is_main_frame`、`headers`（一个 JSON 对象）——请求停住等待 `request-respond`；无默认值、无超时 |
| `request-intercept-closed` | `request_id`（问题还挂着时 browser 关闭；请求随之取消） |
| `cdp-result` | `id`（execute 自己的）、`ok`、`json`（结果或错误字典的文本）、`message`（`cdp-too-large`：应答装不进通道的单行上限） |
| `cdp-event` | `browser`、`method`、`params`（JSON 文本）、`message`（同上）——观察者开着期间该 browser 的 DevTools 后端发出的一切 |
| `loading-progress` | `progress`（0.0–1.0） |
| `fullscreen` | `fullscreen`（bool） |
| `plugin-menu-command` | `slot`、`params`（`page_url`/`link_url`/`source_url`/`selection_text`，非空的才带）——插件的菜单行被点中了 |
| `plugin-message` | `browser`、`reply_id`、`id`、`api`、`args`（JSON 文本）——页面经 chrome.* shim 发起的一次调用；宿主用 `plugin-respond` 应答 |
| `browsers` | `id`（请求的）、`browsers[]`（`id`、`url`、`title`、`active`、`windowed`、`muted`、`zoom`——后两项直接读自该 browser 的 host，不是缓存）——`tabs.query` 用的浏览器表；devtools 与 background 浏览器不在列 |
| `plugin-error` | `id`、`message`（`script-missing:<path>`、`script-too-large:<path>`——这边读不到的内容脚本文件） |
| `status` | `message`（`extension-path-escape`：一条 `cefnest-extension://` 请求落到了 root 之外，已按 403 拒绝） |
| `closed` | — |

凡是携带页面信息的事件（helper → 宿主）都会带 `browser` 字段。只有
`select-browser`、`close-browser`、`load`、`js`、`mute` 与 `cdp-*` 族会读取
该字段；其余命令都作用于当前激活的 browser。

## 四种交互形态

1. **即发即忘**：导航、缩放、静音——发出即完成。
2. **有回执**：`js`/`js-expr` 携带调用方的 `id`，由 `js-result` 应答。注意 `find`
   故意不发送 ack——它的结果全部以 `find-result` 事件送达。
3. **挂起等待应答**：`download-begin` 会把 CEF 的回调挂起在 helper 里然后返回，宿主
   稍后用 `download-accept` / `download-reject` 应答。不应答就不会写入任何文件。
4. **必须应答**：`context-menu`、`permission-request`、`js-dialog`、
   `file-dialog`、`certificate-error` 和 `request-intercept`。沉默并非中立——
   菜单不应答，之后的所有右键都会被阻塞；权限不应答，页面的 promise 会一直
   处于 pending，媒体队列也会堵在它后面；JS 对话框不应答，页面的脚本就停在
   那里；文件选择器不应答，picker 的 promise 一直挂着；证书错误不应答，导航
   停在原地；请求拦截不应答，那一条请求停摆。宿主若放弃对话框类信号，页面也
   不会冻住：`ChromiumSession` 会按 Chrome 的默认值应答（alert 与
   beforeunload：确定；confirm 与 prompt：取消，值为空；文件选择器：取消；
   证书：拒绝）。没有安全默认值的两类——context-menu 与 request-intercept——
   会一直挂到被应答或被收走；helper 自己绝不会背着宿主应答。

`plugin-menu-command` 属"即发即忘"：宿主把它路由给注册了那个 slot 的已加载
原生插件，通道上没有任何一方在等应答。

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
- 对话框的 `id` 由 helper 分配，并在它的整个生命周期内递增；`request-intercept`
  的 `request_id` 是独立的一支序列，应答不会与对话框 id 相撞。CEF 对每个
  browser 串行化 JS 对话框：这边还持有第一个时，第二个会被静默压制、不发
  事件——这与默认 handler 的行为一致。browser 收走的请求以 `js-dialog-closed` /
  `file-dialog-closed` / `certificate-error-closed` / `request-intercept-closed`
  到达，且绝不会被继续——对已经不存在的请求应答只会落空。
- `certificate-authorize` 与拦截规则表只存在于 helper 的内存里：helper 重启后
  两者都从空开始；跨重启的持久化归宿主自己的策略存储。
- `kind` 为 `beforeunload` 的 `js-dialog` 没有 `origin`，`message` 是 Chromium
  自己的文本；页面 handler 里的字符串不会出现在通道上。`file-dialog` 的
  `accept[]` 原样来自 CEF 的过滤表，顺序没有承诺。
- `loading-progress` 是状态而不是增量：最后的 1.0 可能到达不止一次。
- **Match pattern**（菜单行的 `when`；内容脚本将沿用同一语法）：`<all_urls>`，
  或 `scheme://host/path`——`scheme` 是 `*` 或真实协议名，`host` 是 `*`、精确
  主机名或 `*.example.com`（含该域本身与任意子域），`path` 可含 `*` 通配。解析
  不了的模式让该行处处隐藏，而不是处处显示——这是对"没人能解释的条件"唯一
  安全的读法。
