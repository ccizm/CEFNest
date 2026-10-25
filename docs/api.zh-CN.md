# API 速览

[English](api.md) | 简体中文

`ChromiumView` 是控件；`ChromiumSession` 是控制平面，负责启动 helper 并跟踪页面。
本页是速览——权威的逐调用契约，包括下载、查找、右键菜单和权限请求的交互形态，
都写在 [`src/chromium-session.h`](../src/chromium-session.h) 的行内注释里。
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
| 标签页 / 窗口 | `new_browser()` · `select_browser()` · `close_browser()` |
| 查看源码 | `view_source()` → 结果经 `::view-source` / `::view-source-error` 返回；`auto-source-window` 属性（默认开）决定是否自动弹出组件源码窗口 |
| 页面 / 其他 | `set_muted()` · `zoom("in"/"out"/"reset")` · `set_accessibility()` |

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
| `::browser-created` / `::browser-selected` / `::browser-closed` | `id, …` | 标签页与窗口的生命周期 |
| `::popup-requested` | `url` | target=_blank；前台弹窗已就地导航 |
| `::page-message` | `json` | 页面调用了 `cefnestPostMessage()`；用 `run_javascript` 回复 |
| `::primary-selection` / `::copy-text` / `::open-url` | `text` / `text` / `url` | 选区桥接、菜单复制、菜单中的“在新窗口打开” |
| `::zoom-changed` | `level` | 0.0 = 100%；每次主框架加载后也会触发一次 |
| `::view-source` | `browser, url, source` | 一次查看源码请求完成；`source` 仅在发射期间有效 |
| `::view-source-error` | `browser, message` | 源码没能取回 |
| `::closed` | `reason` | helper 已永久退出；要打开新页面需重新调用 `start()` |
| `::helper-exited` | — | 进程已被回收；此时再销毁自己的 display 对象 |
