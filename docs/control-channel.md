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
| `back` `forward` `reload` `stop` | `reload` adds `hard` | navigation; the active browser |
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
| `find` | `pattern`, `forward`, `match_case`, `next` | no ack; answers arrive as `find-result` |
| `find-stop` | `clear` (default true) | ends the search and drops the highlight |
| `view-source` | — | stream the active browser's main frame source back as `view-source-begin`/`-chunk`/`-end`; the menu's View Source takes the same path. No ack — the events are the answer |
| `context-menu-select` | `id`, `flags` (optional) | answers `context-menu`; the helper holds one request at a time |
| `context-menu-dismiss` | — | close the menu, select nothing |
| `permission-respond` | `id`, `result` (`accept`/`deny`/`dismiss`) | answers `permission-request`; a media `accept` must grant the whole mask |
| `js-dialog-respond` | `id`, `ok`, `value` | answers `js-dialog`; `value` is the prompt's text and is ignored by the other kinds |
| `file-dialog-respond` | `id`, `paths[]` | answers `file-dialog`; an empty list is the cancel |
| `certificate-respond` | `id`, `allow` | answers `certificate-error`; the default (an unconnected host) is deny |
| `certificate-authorize` | `fingerprint` | continues this certificate without asking for the helper's lifetime; memory only, persistence is the host's own policy store |
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
| `ready` | `cef`, `pid`, `url` (handshake) |
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
| `load-end` | `url`, `http_code` (**fires on failure too, with code 0**) |
| `load-error` | `error_code`, `url`, `message` |
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

Page-carrying helper → host events all carry a `browser` field. Only
`select-browser`, `close-browser`, `load`, `js`, `mute` and the `cdp-*`
family read one; every other command acts on the active browser.

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
