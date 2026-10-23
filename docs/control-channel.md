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

`DISPLAY` and `WAYLAND_SOCKET` are removed; every other variable belongs to
whoever spawns the helper.

## Commands (host → helper)

| Command | Payload | Notes |
| --- | --- | --- |
| `new-browser` | `url`, `mode` (`tab`/`window`, default `tab`) | a new CEF browser inside the same helper |
| `select-browser` | `browser` | activate the tab; window mode also focuses |
| `close-browser` | `browser` (optional; default active) | close this one only; the last one ends the helper |
| `load` | `url` | the active browser |
| `back` `forward` `reload` `stop` | `reload` adds `hard` | navigation; the active browser |
| `quit` | — | close every browser and end the helper |
| `zoom-in` `zoom-out` `zoom-reset` | — | step 0.5; reset returns to 0.0 |
| `js` | `code`, `id` | statement form, evaluated in the page (needs CSP `unsafe-eval`) |
| `js-expr` | `code`, `id` | expression embedded directly, no `eval` |
| `mute` | `on` (default = toggle) | mute |
| `a11y` | `enabled` | `SetAccessibilityState(STATE_ENABLED/DEFAULT)` |
| `download-accept` | `id`, `path` | absolute path; the host creates parent directories |
| `download-reject` | `id` | refuses an unstarted download, cancels one in flight |
| `download-pause` / `download-resume` | `id` | only for a download that already has its item callback |
| `find` | `pattern`, `forward`, `match_case`, `next` | no ack; answers arrive as `find-result` |
| `find-stop` | `clear` (default true) | ends the search and drops the highlight |
| `context-menu-select` | `id`, `flags` (optional) | answers `context-menu`; the helper holds one request at a time |
| `context-menu-dismiss` | — | close the menu, select nothing |
| `permission-respond` | `id`, `result` (`accept`/`deny`/`dismiss`) | answers `permission-request`; a media `accept` must grant the whole mask |

## Events (helper → host)

| Event | Payload |
| --- | --- |
| `ready` | `cef`, `pid`, `url` (handshake) |
| `browser-created` | `browser`, `url`, `windowed` |
| `browser-selected` | `browser`, `windowed` |
| `browser-closed` | `browser` |
| `address` / `title` | `url` / `title` (`address` only for committed navigations) |
| `loading` | `loading`, `can_go_back`, `can_go_forward` |
| `load-end` | `url`, `http_code` (**fires on failure too, with code 0**) |
| `load-error` | `error_code`, `url`, `message` |
| `renderer-exited` | `status`, `error_code`, `message` (only the renderer's death is reported) |
| `muted` | `muted` (read back) |
| `zoom` | `level` (read back; also once after each main-frame load — Chromium persists zoom per origin) |
| `copy-text` | `text` (the menu's "copy link/image/media address") |
| `open-url` | `url`, `disposition` (the menu's "open in a new window…") |
| `popup-requested` | `url`, `disposition`, `gesture`, `browser` |
| `page-message` | `json` (the raw string from the page's `cefnestPostMessage`) |
| `primary-selection` | `text` (the page's selection; an empty string clears it) |
| `js-result` | `id`, `ok`, `value`, `message` |
| `download-begin` | `id`, `url`, `suggested_name`, `mime_type`, `total_bytes` |
| `download-progress` | `id`, `received_bytes`, `total_bytes`, `percent`, `speed` |
| `download-end` | `id`, `state` (`complete`/`cancelled`/`interrupted`), `path`, `received_bytes` |
| `find-result` | `identifier`, `count`, `active_ordinal`, `final` |
| `context-menu` | `x`, `y`, `items` (each: `id`, `label`, `type`, `enabled`, `checked`) |
| `context-menu-dismissed` | — (only while the helper still holds the callback) |
| `permission-request` | `id`, `kind` (`media`/`prompt`), `origin`, `permissions[]`, `raw` |
| `permission-dismissed` | `id`, `result` (prompt only: media has no dismiss callback) |
| `status` | `message` |
| `closed` | — |

Page-carrying helper → host events all carry a `browser` field. Only
`select-browser` and `close-browser` read one; every other command acts on the
active browser.

## The four interaction shapes

1. **Fire and forget**: navigation, zoom, mute — sent, done.
2. **With a receipt**: `js`/`js-expr` carry a caller `id`, answered by
   `js-result`. Note that `find` deliberately sends no ack — its answers are
   all `find-result` events.
3. **Parked until answered**: `download-begin` parks CEF's callback inside the
   helper and returns; the host answers later with `download-accept` /
   `download-reject`. Unanswered, no file is written.
4. **Must be answered**: `context-menu` and `permission-request`. Silence is
   not neutral — an unanswered menu blocks every later right click, and an
   unanswered permission leaves the page's promise pending with the media
   queue stuck behind it.

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
