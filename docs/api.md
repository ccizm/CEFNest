# API reference

English | [简体中文](api.zh-CN.md)

`ChromiumView` is the widget; `ChromiumSession` is the control plane that
starts the helper and tracks the page. This page is the digest — the
authoritative, per-call contract, including the interaction shapes around
downloads, find, the context menu and permission requests, is documented
inline in [`src/chromium-session.h`](../src/chromium-session.h). Working
samples for every group live in the [examples](../examples/README.md).

**Methods**

| Group | Calls |
| --- | --- |
| Lifecycle | `chromium_session_new()` · `set_view()` · `start()` · `stop()` |
| Data | `set_data_directory()` · `get_data_directory()` · `clear_data()`; the `data-directory` property — see [data-directory.md](data-directory.md) |
| Navigation | `load_uri()` · `go_back()` · `go_forward()` · `reload()` · `stop_loading()` |
| State | `get_uri()` · `get_title()` · `get_loading()` · `get_can_go_back()` · `get_can_go_forward()` · `get_running()` · `get_muted()` · `get_zoom_level()` · `get_frame_count()` · `get_helper_path()` |
| JavaScript | `run_javascript()` · `run_javascript_expression()` → answer on `::script-result` |
| Downloads | answer `::download-begin` with `accept_download()` / `reject_download()`; in flight: `pause_download()` / `resume_download()` |
| Find | `find()` · `stop_finding()` → answers on `::find-result` |
| Context menu | answer `::context-menu` with `context_menu_select()` / `context_menu_dismiss()` |
| Permissions | answer `::permission-request` with `permission_respond()` |
| Tabs / windows | `new_browser()` · `select_browser()` · `close_browser()` |
| View source | `view_source()` → answered by `::view-source` / `::view-source-error`; the `auto-source-window` property (default on) decides whether the component's window opens |
| Page / misc | `set_muted()` · `zoom("in"/"out"/"reset")` · `set_accessibility()` |

**Signals**

| Signal | Carries | Notes |
| --- | --- | --- |
| `::changed` | — | uri / title / loading / back-forward / mute state moved |
| `::ready` | — | helper handshake done |
| `::load-end` | `url, http_code` | fires on failure too, with code 0 |
| `::load-error` | `error_code, url, message` | a navigation that never committed |
| `::renderer-exited` | `status, error_code, message` | the page's render process died |
| `::script-result` | `id, ok, value, message` | answer to a `run_javascript` call |
| `::download-begin` | `id, url, suggested_name, mime_type, total_bytes` | must be answered |
| `::download-progress` / `::download-end` | see header | progress per whole percent |
| `::find-result` | `identifier, count, active_ordinal, final` | one request may answer more than once |
| `::context-menu` | `x, y, items` | return TRUE to draw your own menu, then answer |
| `::permission-request` | `id, kind, origin, permissions, raw` | return TRUE to own the answer; default is deny |
| `::permission-dismissed` | `id, result` | a held prompt was replaced by the page |
| `::browser-created` / `::browser-selected` / `::browser-closed` | `id, …` | tab and window lifecycle |
| `::popup-requested` | `url` | target=_blank; foreground ones already navigated in place |
| `::page-message` | `json` | page called `cefnestPostMessage()`; reply via `run_javascript` |
| `::primary-selection` / `::copy-text` / `::open-url` | `text` / `text` / `url` | selection bridge, menu copy, menu "open in new window" |
| `::zoom-changed` | `level` | 0.0 = 100 %; also after each main-frame load |
| `::view-source` | `browser, url, source` | a View Source request completed; `source` is valid only during the emission |
| `::view-source-error` | `browser, message` | the source never arrived |
| `::closed` | `reason` | helper gone for good; `start()` again for a new page |
| `::helper-exited` | — | the process has been reaped; destroy the display here |
