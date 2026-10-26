# API 速览

[English](api.md) | 简体中文

`ChromiumView` 是控件；`ChromiumSession` 是控制平面，负责启动 helper 并跟踪页面。
本页是速览——权威的逐调用契约，包括下载、查找、右键菜单、权限请求与两类对话框
的交互形态，都写在 [`src/chromium-session.h`](../src/chromium-session.h) 的行内注释里。
每一组 API 都有可运行的示例，见[示例目录](../examples/README.zh-CN.md)。

**方法**

| 分组 | 调用 |
| --- | --- |
| 生命周期 | `chromium_session_new()` · `set_view()` · `start()` · `stop()` |
| 数据目录 | `set_data_directory()` · `get_data_directory()` · `clear_data()`；`data-directory` 属性——见 [data-directory.zh-CN.md](data-directory.zh-CN.md) |
| 导航 | `load_uri()` · `go_back()` · `go_forward()` · `reload()` · `stop_loading()` |
| 状态 | `get_uri()` · `get_title()` · `get_loading()` · `get_can_go_back()` · `get_can_go_forward()` · `get_running()` · `get_muted()` · `get_zoom_level()` · `get_frame_count()` · `get_helper_path()` |
| JavaScript | `run_javascript()` · `run_javascript_expression()` → 结果通过 `::script-result` 返回 |
| 下载 | 用 `accept_download()` / `reject_download()` 应答 `::download-begin`；进行中的下载可用 `pause_download()` / `resume_download()` |
| 查找 | `find()` · `stop_finding()` → 结果通过 `::find-result` 返回 |
| 右键菜单 | 用 `context_menu_select()` / `context_menu_dismiss()` 应答 `::context-menu` |
| 权限 | 用 `permission_respond()` 应答 `::permission-request` |
| 对话框 | 用 `js_dialog_respond()` 应答 `::js-dialog`；用 `file_dialog_respond()` 应答 `::file-dialog` |
| 标签页 / 窗口 | `new_browser()` · `new_background_browser()`（常驻隐藏页：无窗口、无 surface、永不激活） · `select_browser()` · `close_browser()` · `get_browser_surface()`（session 实测的 id→surface 配对） |
| 查看源码 | `view_source()` → 结果经 `::view-source` / `::view-source-error` 返回；`auto-source-window` 属性（默认开）决定是否自动弹出组件源码窗口 |
| 页面 / 其他 | `set_muted()` · `set_zoom_level()` · `set_accessibility()` · `set_proxy()` / `set_proxy_bypass()`（须在 `start()` 之前） · `set_accept_language()`（须在 `start()` 之前） · `set_auto_resize()` / `set_auto_resize_bounds()` |
| Cookies | `cookies_get()` / `cookies_set()` / `cookies_delete()` → 由 `::cookies-result` 返回 |
| 页面内容 | `page_text()` · `history()` · `print_pdf()` → 分别经 `::page-text-*` / `::history-result` / `::pdf-*` 事件族返回 |
| 开发者工具 | `set_devtools_enabled()` · `get_devtools_enabled()`；`devtools-enabled` 属性（默认关）——见[开发者工具](../README.zh-CN.md#开发者工具) |
| CDP 管道 | `cdp_enable(browser)` 打开（每 browser 一支开关） · `cdp_execute(id, browser, method, params_json)` → `::cdp-result` · `cdp_disable(browser)`；观察者开着期间，所有协议事件经 `::cdp-event` 到达 |
| 证书 | 用 `certificate_respond(id, allow)` 应答 `::certificate-error`；`certificate_authorize(fingerprint)` 对该证书在本 helper 生命周期内免询问放行 |
| 请求拦截 | `intercept_add(id, patterns, resource_types)` · `intercept_remove(id)` · `intercept_set_passive()`；用 `request_respond(request_id, action, url)` 应答 `::request-intercept` |
| 插件 | 应答 `::grant-request`（见下） |

**信号**

| 信号 | 参数 | 说明 |
| --- | --- | --- |
| `::changed` | — | URI / 标题 / 加载状态 / 前进后退状态 / 静音状态发生变化 |
| `::ready` | — | helper 握手完成 |
| `::load-end` | `url, http_code` | 失败时也会触发，此时 `http_code` 为 0 |
| `::load-error` | `error_code, url, message` | 一次未能完成提交的导航 |
| `::renderer-exited` | `status, error_code, message` | 页面的渲染进程已退出 |
| `::script-result` | `id, ok, value, message` | `run_javascript` 调用的结果 |
| `::download-begin` | `id, url, suggested_name, mime_type, total_bytes` | 必须应答 |
| `::download-progress` / `::download-end` | 见头文件 | 每跨过一个整数百分比就上报一次进度 |
| `::find-result` | `identifier, count, active_ordinal, final` | 一次请求可能产生多条结果 |
| `::context-menu` | `x, y, items` | 返回 TRUE 表示由你自行绘制菜单，之后必须应答 |
| `::permission-request` | `id, kind, origin, permissions, raw` | 返回 TRUE 表示由你负责应答；默认行为是拒绝 |
| `::permission-dismissed` | `id, result` | 一个挂起的提示已被页面替换 |
| `::js-dialog` | `id, browser, kind, origin, message, default_value` | 返回 TRUE 表示由你自行绘制；不接时 session 按默认应答：alert/beforeunload 确定，confirm/prompt 取消 |
| `::js-dialog-closed` | `id, browser` | browser 收回了挂起的对话框；已经没有可应答的对象 |
| `::file-dialog` | `id, browser, mode, title, default_path, accept` | 返回 TRUE 表示由你自行打开选择器；`accept` 是仅在发射期间有效的 GList；不接时 session 取消 |
| `::file-dialog-closed` | `id, browser` | 选择器还挂着时 browser 关闭了 |
| `::loading-progress` | `progress` | 0.0–1.0；最后的 1.0 可能到达不止一次 |
| `::fullscreen-changed` | `fullscreen` | 页面进入或离开全屏；helper 同时调整它自己的 CEF 窗口 |
| `::browser-created` / `::browser-selected` / `::browser-closed` | `id, …` | 标签页与窗口的生命周期；`browser-created` 附带 `url、windowed、devtools、background` |
| `::browser-surface` | `browser, surface` | session 实测的 id→surface 配对；surface 消失时以 NULL 再发一次 |
| `::popup-requested` | `url` | target=_blank；前台弹窗已就地导航 |
| `::page-message` | `json` | 页面调用了 `cefnestPostMessage()`；用 `run_javascript` 回复 |
| `::primary-selection` / `::copy-text` / `::open-url` | `text` / `text` / `url` | 选区桥接、菜单复制、菜单中的“在新窗口打开” |
| `::zoom-changed` | `level` | 0.0 = 100%；每次主框架加载后也会触发一次 |
| `::view-source` | `browser, url, source` | 一次查看源码请求完成；`source` 仅在发射期间有效 |
| `::view-source-error` | `browser, message` | 源码没能取回 |
| `::closed` | `reason` | helper 已永久退出；要打开新页面需重新调用 `start()` |
| `::helper-exited` | — | 进程已被回收；此时再销毁自己的 display 对象 |
| `::certificate-error` → gboolean | `id, browser, error, url, subject, issuer, fingerprint` | 导航停住等待应答；返回 TRUE 表示由你裁决，返回 FALSE——或不连接——即拒绝 |
| `::certificate-error-closed` | `id, browser` | browser 收回了这个提问 |
| `::request-intercept` | `request_id, browser, url, method, resource_type, is_main_frame, headers` | 请求停住直到 `request_respond()`；无默认值、无超时 |
| `::request-intercept-closed` | `request_id, browser` | 请求还挂着时 browser 关闭了 |
| `::cdp-result` | `id, ok, json, message` | 一次 `cdp_execute()` 的应答；超过通道单行上限时 `message` 为 `cdp-too-large` |
| `::cdp-event` | `browser, method, params, message` | 观察者开着期间的协议事件 |

## 插件

`src/chromium-plugin.h`。**组件层提供模型与机制，这一层什么都不画。** 一个
`ChromiumPlugin` 就是磁盘上一个带清单的目录：它的启用状态、宿主批过的权限、
它的页面落在哪个 URI 上。管理页、设置页、弹出页**怎么显示**——放哪个窗口、
用不用 `GtkPopover`、什么时候关——是宿主应用的决定，因为没有两个应用在这件事上
能达成一致。

| 分组 | 调用 |
| --- | --- |
| 管理器 | `chromium_plugin_manager_new()` · `get_plugins()` → 按 id 排序的 `GListModel`，可直接绑进 `GtkListBox`/`GtkGridView`/`GtkColumnView` · `lookup()`（返回一个新的 reference；被拒收的行也按名字查得到） |
| 发现 | `add_search_path()` · `scan()`——清单被拒的包仍留在列表里，`state=refused`，`error-message` 是机器可读 token（`no-manifest`、`bad-id`、`bad-manifest`、`unsupported-type`、`duplicate-id`、`unsupported-field:<name>`、`popup-size-clamped`） |
| 状态 | `enable()` · `disable()` · `grant()` · `revoke()`；`enabled` 属性写的就是同一台状态机 |
| 扩展页面 | `get_popup_uri()` · `get_options_uri()` · `get_background_uri()`（`background.service_worker` 清单对应的常驻隐藏页；没有则为 NULL） · `get_popup_size()` · `build_uri(self, relative_path, GError **)`——唯一做路径校验的入口，拒绝绝对路径、空串、`.`/`..` 段、反斜杠与百分号 · `icon_path(self, "16")`（磁盘路径：扩展 origin 由 helper 进程提供，本进程取不到那些字节） |
| 会话 | `chromium_session_get_plugin_manager(session)`——session 持有的管理器。它的安装根就是数据目录的 `cefnest-plugins/`（扫描在宿主自加的搜索路径之前；不设数据目录就没有）。启用一个插件会立刻送达运行中的 helper（一条 `plugins-load` 带上 root 与脚本表，每条菜单行一条 `menu-add-user-item`），晚启动的 helper 则在 `::ready` 时补发；卸载走同样的规则 |
| 包 | `install_async(package, id, …)` / `install_finish()`——`.zip` 或 `.crx` 在任务线程解包进安装根，模型在调用方回调之前刷新；`uninstall_async` / `reload_async` 同形。已存在的 id 拒收；被拒清单仍可安装（按 token 落进模型） |
| 应答与动作 | `respond(reply_id, ok, value)`——对一条认领的 `::plugin-message` 的应答 · `activate_action(id)`——宿主的按钮按下；`::plugin-action` 发出，插件自己的监听器同时听见 |

| 信号 | 携带 | 说明 |
| --- | --- | --- |
| `::changed` | — | 插件集合，或某个插件的状态动了 |
| `::grant-request` | `id, permissions` | `enable()` 撞到"清单声明了、表里还没有"时发出。**没有返回值**：答案就是权限表——在发射返回前调 `grant()`，这次 `enable()` 直接成功；什么都不做则以 `G_IO_ERROR_PERMISSION_DENIED` 失败。没人应答的请求等于拒绝 |
| `::plugin-error` | `id, message` | 拒收 token、`unsupported-field:<name>`、加载失败（`library-open-failed`、`library-symbol-missing`）、helper 侧脚本失败（`script-missing:<path>`）、权限拒绝（`permission-denied:storage`） |
| `::plugin-message` → gboolean | `id, api, args, reply_id` | 页面经 chrome.* shim 发起的一次调用。返回 TRUE 即认领（内建 handler 退让；用 `respond()` 应答），FALSE 交给它们。应答可以晚很久——`respond()` 以 `reply_id` 为键 |
| `::plugin-action` | `id` | 宿主为 `id` 摆的按钮按下了；显示什么由宿主的策略决定 |

### 原生前端

清单写 `"cefnest": {"type": "native", "library": "plugin.so"}` 的包，在
enable 时把那个 `.so` 加载进宿主进程，调用它唯一的导出符号
`cefnest_plugin_register(registry, api_version)`，disable 时卸载——并按宿主
自己的记账把它注册过的东西全部回收：

| Registry 动词 | 作用 |
| --- | --- |
| `add_menu_item(label, slot, callback, user_data)` | 加一条右键菜单行；`slot` 取 0–199，映射为菜单 id 26600+slot，行被点中时回调收到 `(slot, params_json, user_data)` |
| `subscribe(detailed_signal, callback, user_data)` | 按名字订阅 session 信号（`load-end`、`browser-created`……），或由 registry 自己投递的 `"plugin-storage"` |
| `set_action(button)` | 把一个 widget 交给宿主摆 |
| `storage_get(key)` / `storage_set(key, json)` | 每插件独立的存储；有数据目录落盘之前只存在于内存 |

所有回调都跑在宿主主线程上。加载用 `g_module_open`，插件只依赖公共头——
它链接的只有自己的包含，registry 调用经宿主自身的符号表解析。插件是
宿主进程里的代码，没有沙箱：只安装你自己构建的插件。
`examples/plugin-demo/` 是一个完整的例子，`test/plugin-gate.sh` 对它做端到端
门禁（菜单行、回调、越界拒绝、`G_DEBUG=fatal-criticals` 下的卸载与重载）。

### Web 前端

Web 插件的内容脚本在 `document_start` 运行（先于页面自己的内联脚本——实测）、
`document_end`/`document_idle` 在 load end 运行。组件在一根桥函数之上提供
`chrome.*` shim；宿主按权限表裁决每一次调用。子集、接缝、载荷上限与隔离
事实都在 [docs/plugins.zh-CN.md](plugins.zh-CN.md)——写插件前先读它。响应
过滤（拦截二期）规划在
[`docs/plans/response-filter-plan-2026-10-26.md`](plans/response-filter-plan-2026-10-26.md)。
