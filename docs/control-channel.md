# Control channel protocol

English | [简体中文](control-channel.zh-CN.md)

`ChromiumSession` and the helper talk over a unix socket carrying NDJSON: the
host sends commands (`{"cmd": …, …}`), the helper sends events
(`{"event": …, …}`), one JSON object per line. The `ChromiumSession` API and
signals cover everything a normal host needs; this page matters only when you
write your own helper or drive the channel directly.

## Helper startup environment

Set by the host when it spawns the subprocess:

| Variable | Value |
| --- | --- |
| `WAYLAND_DISPLAY` | the view's nested compositor socket name |
| `GDK_BACKEND` | `wayland` |
| `XDG_SESSION_TYPE` | `wayland` |
| `CHROMIUM_HELPER_SOCKET` | full path of the control-channel socket |
| `CHROMIUM_HELPER_DATA_DIR` | absolute path of a persistent profile; absent means incognito |
| `CHROMIUM_HELPER_SCHEMES` | the host's custom schemes, `name=flag,flag;name2` with flags `standard` `local` `display_isolated` `secure` `cors` `fetch` `csp_bypassing` (empty flags = `standard,secure,fetch`); read in every process by `OnRegisterCustomSchemes`, the one point CEF gives scheme options a voice. Absent means no host schemes |

`DISPLAY` and `WAYLAND_SOCKET` are removed. `CHROMIUM_HELPER_DATA_DIR` is set
only when the host asked for a data directory — otherwise it is removed even
if the host's own environment had it — and every other variable belongs to
whoever spawns the helper. A helper started by hand reads the same variable:
an absolute path selects a persistent profile, absence selects incognito with
a private root under `$XDG_RUNTIME_DIR` (or `/dev/shm`).

## Commands (host → helper)

| Command | Payload | Notes |
| --- | --- | --- |
| `new-browser` | `url`, `mode` (`tab`/`window`/`background`, default `tab`) | a new CEF browser inside the same helper; `background` runs the page with no window and no surface (a plugin manifest's `background.service_worker` rides on it) |
| `select-browser` | `browser` | activate the tab; window mode also focuses |
| `close-browser` | `browser` (optional; default active) | close this one only; the last one ends the helper |
| `load` | `url` | the active browser |
| `back` `forward` `reload` `stop` | `reload` adds `hard`; all five take an optional `browser` (default active) | navigation, nameable at a background tab |
| `quit` | — | close every browser and end the helper |
| `zoom-in` `zoom-out` `zoom-reset` | — | step 0.5; reset returns to 0.0 |
| `js` | `code`, `id` | statement form, evaluated in the page (needs CSP `unsafe-eval`) |
| `js-expr` | `code`, `id` | expression embedded directly, no `eval` |
| `mute` | `browser` (optional), `on` (default = toggle) | mute; `browser` names the tab (`tabs.update`'s muted path) and the `muted` event names the browser actually muted |
| `a11y` | `enabled` | `SetAccessibilityState(STATE_ENABLED/DEFAULT)` |
| `devtools` | `enabled` | offers 检查 in the context menu and F12 / Ctrl+Shift+I; the inspector browser arrives as `browser-created` with `devtools` true |
| `download-accept` | `id`, `path` | absolute path; the host creates parent directories |
| `download-reject` | `id` | refuses an unstarted download, cancels one in flight |
| `download-pause` / `download-resume` | `id` | only for a download that already has its item callback |
| `find` | `pattern`, `forward`, `match_case`, `next`, optional `browser` | no ack; answers arrive as `find-result` |
| `find-stop` | `clear` (default true), optional `browser` | ends the search and drops the highlight |
| `view-source` | optional `browser` | stream that browser's main frame source back as `view-source-begin`/`-chunk`/`-end`; the menu's View Source takes the same path. No ack — the events are the answer |
| `history-go` | `index`, optional `browser` | the one history jump CEF's public API has no button for: the helper runs `Page.getNavigationHistory` + `Page.navigateToHistoryEntry` over its own DevTools pipe — the host's `cdp-enable` is not consulted, this is a scoped capability. The jump lands as the usual `address`/`load-*` events; no receipt |
| `zoom-set` | `level`, optional `browser` | one explicit CEF zoom level (0.0 = 100 %); the zoom family and `stop` all take the optional `browser` too |
| `ua-set` | `value`, optional `browser` | the user agent. Without `browser` this is the session-wide property (every current browser hears it; future ones at creation); with one, that browser's own override wins. Empty value = back to Chromium's own (captured via `Browser.getVersion`). Applied per load start, so a renderer crash does not lose it |
| `context-menu-select` | `id`, `flags` (optional) | answers `context-menu`; the helper holds one request at a time |
| `context-menu-dismiss` | — | close the menu, select nothing |
| `permission-respond` | `id`, `result` (`accept`/`deny`/`dismiss`) | answers `permission-request`; a media `accept` must grant the whole mask |
| `js-dialog-respond` | `id`, `ok`, `value` | answers `js-dialog`; `value` is the prompt's text and is ignored by the other kinds |
| `file-dialog-respond` | `id`, `paths[]` | answers `file-dialog`; an empty list is the cancel |
| `certificate-respond` | `id`, `allow` | answers `certificate-error`; the default (an unconnected host) is deny |
| `certificate-authorize` | `fingerprint` | continues this certificate without asking for the helper's lifetime; memory only, persistence is the host's own policy store |
| `auth-respond` | `id`, `user`, `password` | answers `auth-required`: the request continues with these credentials |
| `auth-cancel` | `id` | answers `auth-required` with a cancel — the request fails with its 401, Chrome's own default |
| `client-cert-respond` | `id`, `index` | answers `client-cert-request`; -1 continues the handshake without a certificate, which is also the default |
| `tls-info` | `id` (the host's own), optional `browser` | one read of the loaded page's certificate facts, over the component's own pipe (`Security.enable` + `visibleSecurityStateChanged`; CEF 156 has no `getVisibleSecurityState`). Answered by `tls-info`; a question nothing answers in five seconds reports `ok` false |
| `capture-screenshot` | `id` (the host's own), optional `browser` | one PNG of the page (`Page.captureScreenshot`), answered by `screenshot-result`; a capture past ~3 MiB is refused as `screenshot-too-large` rather than sent unreadable |
| `clear-data` | `types` (bitmask: 1 cache, 2 cookies, 4 HTTP auth cache, 8 origin storage), `origins[]` (origin storage only) | typed clearing over the same pipe (`Network.clearBrowserCache` / `clearBrowserCookies` / `clearHttpAuthCache`, `Storage.clearDataForOrigin` per origin). Fire-and-forget; each call logs its own outcome |
| `script-add` | `id`, `patterns[]`, `code`, `run_at` (`document_start`/`document_end`/`document_idle`, default idle), `all_frames`, optional `browser` (default all browsers) | registers a host script; re-adding an id replaces it. `document_start` injects in the renderer before the page's inline scripts; the other two ride main-frame load end. There is no isolated world — the script runs in the page's own JavaScript domain |
| `script-remove` | `id` | takes the script back out |
| `scheme-register` | `name`, `flags` (optional, for the next start) | attaches the host's data factory for this scheme at once; the *options* are read from `CHROMIUM_HELPER_SCHEMES` at process start only, and a `flags` payload here is logged as deferred. Names must match `[a-z][a-z0-9+.-]*`; built-ins and `cefnest-extension` are refused |
| `scheme-unregister` | `name` | detaches the factory |
| `scheme-response` | `id`, `status`, `reason`, `content_type`, `headers` (optional JSON object) | answers `scheme-request`: names the response, opens the body |
| `scheme-chunk` | `id`, `data` (base64, 48 KiB raw per chunk) | one body piece |
| `scheme-end` | `id` | closes the body |
| `scheme-abort` | `id` | the host takes the request back; the load cancels |
| `intercept-add` | `id`, `patterns[]` (match patterns), `resource_types[]` (optional: `main_frame`, `image`, `xhr`, …) | registers one interception rule under the host's own id; re-adding an id replaces it. No rules registered = the feature is off and costs nothing |
| `intercept-remove` | `id` | takes the rule back out |
| `intercept-mode` | `passive` | passive reports every match and blocks nothing; the default is blocking |
| `request-respond` | `request_id`, `action` (`allow`/`cancel`/`redirect`), `url` (redirect only) | answers `request-intercept`; a redirect swaps the URL first. No timeout — the page waits for the host |
| `cdp-enable` | `browser` (optional) | registers the DevTools protocol observer on this browser; the one switch that lets `cdp-execute` through |
| `cdp-disable` | `browser` (optional) | unregisters the observer |
| `cdp-execute` | `id` (the host's own), `method`, `params` (JSON text) | one DevTools method call; refused with `cdp-not-enabled` while the observer is off. Answered by `cdp-result` |
| `plugins-load` | `plugins[]`: `id`, `root`, `background` (optional `background.service_worker` path), `scripts[]` (`matches`, `js`, `run_at`) | the whole truth about one enabled plugin: registers its root on `cefnest-extension://` (the factory serves only these), stores its content scripts, and names the script the synthesized `__cefnest_background__.html` page runs. Sending it again for the same id replaces that entry — the helper keeps no history |
| `plugins-unload` | `id` | drops the plugin's root and scripts |
| `menu-add-user-item` | `slot` (0–199), `label`, `when` (optional match pattern) | one context-menu row at id 26600+slot, appended after everything else when the menu model is built; `when` hides the row on pages the pattern does not match |
| `menu-remove-user-item` | `slot` | takes the row back (disable/unload) |
| `plugin-respond` | `browser`, `reply_id`, `ok`, `value` (JSON text) | the host's answer to one `plugin-message`; routed to that browser's renderer, delivered to the shim holding the reply id. `value` past 1 MiB is refused with `plugin-payload-too-large` |
| `browsers-list` | `id` (request id) | asks for the browser table; answered by the `browsers` event with the same `id` |

## Events (helper → host)

| Event | Payload |
| --- | --- |
| `ready` | `version` (the control-channel contract this helper speaks; the session stops a helper whose major differs), `cef`, `pid`, `url` (handshake) |
| `browser-created` | `browser`, `url`, `windowed`, `devtools`, `background` (the inspector CEF created for `ShowDevTools`: always windowed, CEF owns its window, and it never becomes the active browser; `background` TRUE marks the resident hidden page — no surface, never active, never in `browsers`) |
| `browser-selected` | `browser`, `windowed` |
| `browser-closed` | `browser` |
| `take-focus` | `browser`, `next` (the page's Tab traversal ran off the edge of the document; the host answers by handing the focus chain to the widget after — `next` false: before — the view) |
| `view-source-begin` | `browser`, `id`, `url`, `title`, `bytes` (the source is the serialized DOM, script changes included, not the network's original bytes) |
| `view-source-chunk` | `browser`, `id`, `data` (64 KiB per chunk, whole UTF-8; ordered) |
| `view-source-end` | `browser`, `id`, `truncated` (the helper caps at 16 MiB; a second `begin` abandons the transfer in flight) |
| `view-source-error` | `browser`, `id`, `message` (alone, or after a `begin` — the frame can be gone before anything streamed) |
| `address` / `title` | `url` / `title` (`address` only for committed navigations) |
| `loading` | `loading`, `can_go_back`, `can_go_forward` |
| `load-started` | `browser`, `url`, `transition` (Chromium's bitmask), `is_main_frame` — one per committed navigation; a navigation that fails before commit never reaches here |
| `load-end` | `browser`, `url`, `http_code` (**fires on failure too, with code 0**) — exactly one per navigation |
| `load-error` | `browser`, `error_code`, `url`, `message` — exactly one per failed navigation: Chromium's ERR_ABORTED echo, which lands seconds behind the real error, is dropped by the helper |
| `renderer-exited` | `status`, `error_code`, `message` (only the renderer's death is reported) |
| `muted` | `browser`, `muted` (read back; `browser` names the tab actually muted — a background tab going quiet says nothing about the active view) |
| `zoom` | `level` (read back; also once after each main-frame load — Chromium persists zoom per origin) |
| `copy-text` | `text` (the menu's "copy link/image/media address") |
| `open-url` | `url`, `disposition` (the menu's "open in a new window…") |
| `popup-requested` | `url`, `disposition`, `gesture`, `browser` |
| `page-message` | `json` (the raw string from the page's `cefnestPostMessage`) |
| `primary-selection` | `text` (the page's selection; an empty string clears it) |
| `js-result` | `id`, `ok`, `value`, `message` (`ok` is TRUE only when the script ran *and* its result was delivered; a result over the helper's 64 KiB ceiling arrives as `ok` false, `value` null, `message` naming the drop) |
| `download-begin` | `id`, `url`, `suggested_name`, `mime_type`, `total_bytes` |
| `download-progress` | `id`, `received_bytes`, `total_bytes`, `percent`, `speed` |
| `download-end` | `id`, `state` (`complete`/`cancelled`/`interrupted`), `path`, `received_bytes` |
| `find-result` | `identifier`, `count`, `active_ordinal`, `final` |
| `context-menu` | `x`, `y`, `items` (each: `id`, `label`, `type`, `enabled`, `checked`) |
| `context-menu-dismissed` | — (only while the helper still holds the callback) |
| `permission-request` | `id`, `kind` (`media`/`prompt`), `origin`, `permissions[]`, `raw` |
| `permission-dismissed` | `id`, `result` (prompt only: media has no dismiss callback) |
| `js-dialog` | `id`, `kind` (`alert`/`confirm`/`prompt`/`beforeunload`), `origin`, `message`, `default_value` |
| `js-dialog-closed` | `id` (a held dialog the browser took away: a navigation reset or a close) |
| `file-dialog` | `id`, `mode` (`open`/`open-multiple`/`open-folder`/`save`), `title`, `default_path`, `accept[]` |
| `file-dialog-closed` | `id` (the browser closed while the chooser was held) |
| `certificate-error` | `id`, `error` (Chromium's net-error name), `url`, `subject`, `issuer`, `fingerprint` (SHA-256 of the leaf's DER, hex) — the navigation is stopped until answered; default deny |
| `certificate-error-closed` | `id` (the browser navigated or closed while the question was parked) |
| `auth-required` | `id`, `browser`, `is_proxy`, `scheme` (`basic`/`digest`/…), `realm`, `origin` — the request is stopped until `auth-respond`/`auth-cancel`; the session cancels when no host listens, which is the request dying with its 401 |
| `auth-closed` | `id`, `browser` (the browser navigated or closed while the question was parked) |
| `client-cert-request` | `id`, `browser`, `origin` (`host:port`), `certs[]` (`subject`, `issuer`, `fingerprint`) — the handshake is stopped until `client-cert-respond`; default none |
| `client-cert-closed` | `id`, `browser` |
| `tls-info` | `id` (the ask's), `browser`, `ok`, `security_state` (`secure`/`insecure`/`insecure-broken`/…), `subject`, `issuer`, `fingerprint` (SHA-256 of the leaf's DER, hex; "" when the state carries none), `valid_from`, `valid_to` (Unix seconds) |
| `screenshot-result` | `id`, `browser`, `ok`, `data` (base64 PNG; present when `ok`), `message` (`screenshot-too-large` on the ceiling) |
| `window-properties` | `browser`, `x`, `y`, `w`, `h`, `toolbar`, `menubar`, `status`, `resizable`, `fullscreen` — a windowed browser's window at creation and on bounds/fullscreen changes; this helper's windows are frameless, so toolbar/menubar/status are always false |
| `browsers-changed` | `browsers[]` (`id`, `url`, `title`, `active`, `windowed`, `muted`, `zoom` — the whole table, pushed on create, close, select, title, address, mute and zoom) |
| `scheme-request` | `id`, `browser`, `url`, `method`, `headers` (one JSON object) — the load is stopped until `scheme-response`/`scheme-abort`; the session aborts when no host listens, because the component does not invent page data |
| `scheme-closed` | `id`, `browser` (the browser went away mid-request) |
| `request-intercept` | `request_id`, `url`, `method`, `resource_type`, `is_main_frame`, `headers` (one JSON object) — the request is stopped until `request-respond`; no default, no timeout |
| `request-intercept-closed` | `request_id` (the browser closed while the question was parked; the request was cancelled with it) |
| `cdp-result` | `id` (the execute's own), `ok`, `json` (the result or error dictionary as text), `message` (`cdp-too-large` when the answer could not fit the channel's line) |
| `cdp-event` | `browser`, `method`, `params` (JSON text), `message` (`cdp-too-large` likewise) — everything the browser's DevTools backend emits while the observer is on |
| `loading-progress` | `progress` (0.0–1.0) |
| `fullscreen` | `fullscreen` (bool) |
| `plugin-menu-command` | `slot`, `params` (`page_url`/`link_url`/`source_url`/`selection_text`, whichever are non-empty) — a plugin's menu row was picked |
| `plugin-message` | `browser`, `reply_id`, `id`, `api`, `args` (JSON text) — one call from a page through the chrome.* shim; the host answers with `plugin-respond` |
| `browsers` | `id` (the request's), `browsers[]` (`id`, `url`, `title`, `active`, `windowed`, `muted`, `zoom` — the last two read off the browser's host, not from a cache) — the browser table for `tabs.query`; devtools and background browsers are not listed |
| `plugin-error` | `id`, `message` (`script-missing:<path>`, `script-too-large:<path>` — a content script file this side could not read) |
| `status` | `message` (`extension-path-escape`: a `cefnest-extension://` request landed outside its root and was refused with 403) |
| `closed` | — |

Page-carrying helper → host events all carry a `browser` field. The
commands that read one: `select-browser`, `close-browser`, `load`, `js`,
`mute`, the whole navigation, zoom and find families, `view-source`,
`history`, `page-text`, `print-pdf`, `history-go`, `tls-info`,
`capture-screenshot`, `ua-set` (where absent means the session-wide form),
`script-add` (absent = all browsers) and the `cdp-*` family. Every other
command acts on the active browser.

## The four interaction shapes

1. **Fire and forget**: navigation, zoom, mute — sent, done.
2. **With a receipt**: `js`/`js-expr` carry a caller `id`, answered by
   `js-result`. Note that `find` deliberately sends no ack — its answers are
   all `find-result` events.
3. **Parked until answered**: `download-begin` parks CEF's callback inside the
   helper and returns; the host answers later with `download-accept` /
   `download-reject`. Unanswered, no file is written.
4. **Must be answered**: `context-menu`, `permission-request`, `js-dialog`,
   `file-dialog`, `certificate-error` and `request-intercept`. Silence is not
   neutral — an unanswered menu blocks every later right click, an unanswered
   permission leaves the page's promise pending with the media queue stuck
   behind it, an unanswered JS dialog stops the page's script, an unanswered
   file dialog leaves the picker pending, an unanswered certificate error
   stops the navigation, and an unanswered interception stalls that one
   request. A host that declines the dialog events does not freeze the page:
   `ChromiumSession` answers with Chrome's defaults (alert and beforeunload:
   ok, confirm and prompt: cancel with an empty value, file dialog: cancel,
   certificate: deny). The two without a safe default — context-menu and
   request-intercept — stay held until answered or taken away; the helper
   itself never answers behind the host's back.

`plugin-menu-command` is fire-and-forget: the host routes it to whichever
loaded native plugin registered that slot, and nothing on the channel waits
for an answer.

## Channel rules

- One JSON object per line. The host's read side caps a line at **4 MiB** and
  tears the connection down past it; a line containing a NUL byte is dropped
  whole.
- A missing field folds to that type's empty value (string → `""`, numbers and
  bools → `0`/`FALSE`, byte counts → `-1`) — no warning, no rejection. The
  helper is entitled to omit a field.
- Byte counts travel as JSON doubles (exact below 2^53;
  `CefDictionaryValue` only has 32-bit setters).
- A `download-*` `id` is Chromium's download number and increases across
  sessions; a `find-result` `identifier` increments once per request and only
  groups the replies belonging to that request.
- `total_bytes`/`percent` of `-1` means the server stated no length; an empty
  `download-end` `path` means the download was rejected before it started.
- A dialog `id` is assigned by the helper and increases for its lifetime; a
  `request-intercept`'s `request_id` is its own sequence, so answers never
  collide with dialog ids. CEF serialises JS dialogs per browser: a second
  one while this side holds the first is suppressed with no event, which is
  the default handler's own behaviour. A request the browser takes away
  arrives as `js-dialog-closed` / `file-dialog-closed` /
  `certificate-error-closed` / `request-intercept-closed` and is never
  continued — the answer would cross a request that no longer exists.
- `certificate-authorize` and the interception rule table live only in the
  helper's memory: a helper restart starts both empty, and persistence across
  restarts is the host's own policy store.
- A `js-dialog` of kind `beforeunload` carries no `origin` and Chromium's own
  message; the page's handler string is not what travels. `file-dialog`
  `accept[]` is CEF's filter list verbatim, with no order promise.
- `loading-progress` is state, not a delta: the final 1.0 can arrive more than
  once.
- **Match patterns** (the `when` of a menu row; the same syntax content
  scripts will use): `<all_urls>`, or `scheme://host/path` where `scheme` is
  `*` or a real one, `host` is `*`, an exact host, or `*.example.com` (the
  domain itself and any subdomain), and `path` may contain `*` wildcards. A
  pattern that does not parse hides the row everywhere rather than showing it
  everywhere — the safe reading of a condition nobody could honour.
