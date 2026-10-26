# API reference

English | [简体中文](api.zh-CN.md)

`ChromiumView` is the widget; `ChromiumSession` is the control plane that
starts the helper and tracks the page. This page is the digest — the
authoritative, per-call contract, including the interaction shapes around
downloads, find, the context menu, permission requests and the two dialog
kinds, is documented inline in
[`src/chromium-session.h`](../src/chromium-session.h). Working
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
| Dialogs | answer `::js-dialog` with `js_dialog_respond()`; `::file-dialog` with `file_dialog_respond()` |
| Tabs / windows | `new_browser()` · `new_background_browser()` (a resident hidden page: no window, no surface, never active) · `select_browser()` · `close_browser()` · `get_browser_surface()` (the session's measured id→surface pairing) |
| View source | `view_source()` → answered by `::view-source` / `::view-source-error`; the `auto-source-window` property (default on) decides whether the component's window opens |
| Page / misc | `set_muted()` · `set_zoom_level()` · `set_accessibility()` · `set_proxy()` / `set_proxy_bypass()` (before `start()`) · `set_accept_language()` (before `start()`) · `set_auto_resize()` / `set_auto_resize_bounds()` |
| Cookies | `cookies_get()` / `cookies_set()` / `cookies_delete()` → answered by `::cookies-result` |
| Page content | `page_text()` · `history()` · `print_pdf()` → answered by the `::page-text-*` / `::history-result` / `::pdf-*` families |
| Developer tools | `set_devtools_enabled()` · `get_devtools_enabled()`; the `devtools-enabled` property (default off) — see [Developer tools](../README.md#developer-tools) |
| CDP pipe | `cdp_enable(browser)` opens it (one switch, per browser) · `cdp_execute(id, browser, method, params_json)` → `::cdp-result` · `cdp_disable(browser)`; every protocol event arrives on `::cdp-event` while the observer is on |
| Certificates | answer `::certificate-error` with `certificate_respond(id, allow)`; `certificate_authorize(fingerprint)` trusts one certificate for this helper's lifetime |
| Interception | `intercept_add(id, patterns, resource_types)` · `intercept_remove(id)` · `intercept_set_passive()`; answer `::request-intercept` with `request_respond(request_id, action, url)` |
| Plugins | answer `::grant-request` (see below) |

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
| `::js-dialog` | `id, browser, kind, origin, message, default_value` | return TRUE to draw your own; declined, the session answers alert/beforeunload ok and confirm/prompt cancel |
| `::js-dialog-closed` | `id, browser` | the browser dropped a held dialog; nothing is left to answer |
| `::file-dialog` | `id, browser, mode, title, default_path, accept` | return TRUE to draw a chooser; `accept` is a GList valid only during the emission; declined, the session cancels |
| `::file-dialog-closed` | `id, browser` | the browser closed under a held chooser |
| `::loading-progress` | `progress` | 0.0–1.0; the final 1.0 may arrive more than once |
| `::fullscreen-changed` | `fullscreen` | the page entered or left fullscreen; the helper also resizes its own CEF window |
| `::browser-created` / `::browser-selected` / `::browser-closed` | `id, …` | tab and window lifecycle; `browser-created` carries `url, windowed, devtools, background` |
| `::browser-surface` | `browser, surface` | the session's measured id→surface pairing; NULL when the surface goes away |
| `::popup-requested` | `url` | target=_blank; foreground ones already navigated in place |
| `::page-message` | `json` | page called `cefnestPostMessage()`; reply via `run_javascript` |
| `::primary-selection` / `::copy-text` / `::open-url` | `text` / `text` / `url` | selection bridge, menu copy, menu "open in new window" |
| `::zoom-changed` | `level` | 0.0 = 100 %; also after each main-frame load |
| `::view-source` | `browser, url, source` | a View Source request completed; `source` is valid only during the emission |
| `::view-source-error` | `browser, message` | the source never arrived |
| `::closed` | `reason` | helper gone for good; `start()` again for a new page |
| `::helper-exited` | — | the process has been reaped; destroy the display here |
| `::certificate-error` → gboolean | `id, browser, error, url, subject, issuer, fingerprint` | the navigation is stopped until answered; return TRUE to decide, FALSE — or nothing connected — denies |
| `::certificate-error-closed` | `id, browser` | the browser took the question away |
| `::request-intercept` | `request_id, browser, url, method, resource_type, is_main_frame, headers` | the request is stopped until `request_respond()`; no default, no timeout |
| `::request-intercept-closed` | `request_id, browser` | the browser closed under a held request |
| `::cdp-result` | `id, ok, json, message` | the answer to one `cdp_execute()`; `message` is `cdp-too-large` past the channel's line limit |
| `::cdp-event` | `browser, method, params, message` | protocol events, while the observer is on |

## Plugins

`src/chromium-plugin.h`. **The component provides the model and the mechanism;
nothing in it draws.** A `ChromiumPlugin` is a directory on disk with a
manifest in it: its enabled state, the permissions the host granted, and the
URIs its pages live on. How a management page, an options page or a popup is
displayed — which window, which popover, when it closes — is the embedding
application's decision, because no two applications agree on it.

| Group | Calls |
| --- | --- |
| Manager | `chromium_plugin_manager_new()` · `get_plugins()` → a `GListModel` ordered by id, bindable straight into `GtkListBox`/`GtkGridView`/`GtkColumnView` · `lookup()` (returns a new reference; refused rows are found by name too) |
| Discovery | `add_search_path()` · `scan()` — a refused manifest stays in the list as `state=refused` with `error-message` set to a machine token (`no-manifest`, `bad-id`, `bad-manifest`, `unsupported-type`, `duplicate-id`, `unsupported-field:<name>`, `popup-size-clamped`) |
| State | `enable()` · `disable()` · `grant()` · `revoke()`; the `enabled` property writes through the same machine |
| Extension pages | `get_popup_uri()` · `get_options_uri()` · `get_background_uri()` (the resident hidden page a `background.service_worker` manifest runs; NULL without one) · `get_popup_size()` · `build_uri(self, relative_path, GError **)` — the only path-validating entry, refusing absolute paths, empty strings, `.`/`..` segments, backslashes and percent signs · `icon_path(self, "16")` (a disk path: the extension origin is served inside the helper, so this process cannot fetch it) |
| Session | `chromium_session_get_plugin_manager(session)` — the manager owned by the session. Its install root is the data directory's `cefnest-plugins/` (scanned before host-added search paths; absent when no data directory is set). Enabling a plugin reaches a running helper at once (one `plugins-load` command with its root and script table, `menu-add-user-item` per menu row) and a helper started later at `::ready`; the same rules unload it |
| Packages | `install_async(package, id, …)` / `install_finish()` — a `.zip` or `.crx` unpacked into the install root on a task thread, the model refreshed before the caller's callback; `uninstall_async` / `reload_async` in the same shape. An id that exists refuses; a refused manifest still installs (it lands in the model with its token) |
| Answers & actions | `respond(reply_id, ok, value)` — the answer to a claimed `::plugin-message` · `activate_action(id)` — the host's button; `::plugin-action` goes out and the plugin's own listeners hear it |

| Signal | Carries | Notes |
| --- | --- | --- |
| `::changed` | — | the set, or one plugin's state |
| `::grant-request` | `id, permissions` | emitted by `enable()` for declared names the table lacks. **No return value**: the grant table is the answer — call `grant()` before the emission returns and that same `enable()` succeeds; do nothing and it fails with `G_IO_ERROR_PERMISSION_DENIED`. An unanswered request is a deny |
| `::plugin-error` | `id, message` | a refusal token, `unsupported-field:<name>`, a load failure (`library-open-failed`, `library-symbol-missing`), a helper-side script failure (`script-missing:<path>`), or a permission refusal (`permission-denied:storage`) |
| `::plugin-message` → gboolean | `id, api, args, reply_id` | one call from a page through the chrome.* shim. Return TRUE to claim it (the built-in handlers step aside; answer with `respond()`), FALSE to leave it to them. The reply may come long after — respond() keys on `reply_id` |
| `::plugin-action` | `id` | the host's button for `id` went down; what shows is the host's policy |

### The native frontend

A manifest with `"cefnest": {"type": "native", "library": "plugin.so"}` loads
that `.so` into the host process on enable, calls its one export —
`cefnest_plugin_register(registry, api_version)` — and unmaps it on disable,
undoing everything it registered from the host's own ledger:

| Registry verb | What it does |
| --- | --- |
| `add_menu_item(label, slot, callback, user_data)` | one context-menu row; `slot` 0–199 maps to menu id 26600+slot, and the callback runs with `(slot, params_json, user_data)` when the row is picked |
| `subscribe(detailed_signal, callback, user_data)` | a session signal by name (`load-end`, `browser-created`, …), or the registry-delivered `"plugin-storage"` |
| `set_action(button)` | hands the host a widget the host may place |
| `storage_get(key)` / `storage_set(key, json)` | per-plugin storage, in memory until a data directory backs it |

The callbacks all run on the host's main thread. Loading is
`g_module_open` against the public header only — the plugin links nothing but
its includes and calls the registry through the host's own symbols. A plugin
is host-process code with no sandbox: install only ones you built.
`examples/plugin-demo/` is a complete one, and `test/plugin-gate.sh` drives
it end to end (menu row, callback, escape refusals, unload-and-reload under
`G_DEBUG=fatal-criticals`).

### The Web frontend

A web plugin's content scripts run at `document_start` (before the page's own
inline scripts — measured), `document_end`/`document_idle` at load end. The
component provides the `chrome.*` shim on top of one bridge function; the
host adjudicates every call against the permission table. The subset, its
seams, the payload ceiling and the isolation facts are in
[docs/plugins.md](plugins.md) — read that before writing a plugin. The
remaining work (surface pairing, background pages, `tabs` expansion) is
planned in
[`docs/plans/work-plan-2026-10-26.md`](plans/work-plan-2026-10-26.md).

