# Plugins

[English](plugins.md) | 简体中文

What a plugin is, what one can do, and what it can never do. The method and
signal digest is in [api.md](api.md); response filtering (interception phase
two) is planned in
[`docs/plans/response-filter-plan-2026-10-26.md`](plans/response-filter-plan-2026-10-26.md).

## The shape

One component, two frontends. A plugin is a directory with a
`cefnest-plugin.json` manifest in it; `"cefnest": {"type": "native"}` loads a
`.so` into the host process, `"type": "web"` runs content scripts and the
`chrome.*` subset on the page. The manifest is shape-compatible with
`manifest_version: 3` — the field names and their meanings are Chrome's — but
**this is not a Chrome extension runtime**: there is no service worker, no
isolated world, no extension processes, and no store. Compatibility means a
package written against MV3's *shape* describes itself correctly here, not
that a store extension will run.

The manifest:

```json
{
  "manifest_version": 3,
  "name": "Example",
  "version": "1.0",
  "description": "…",
  "permissions": ["storage", "contextMenus"],
  "content_scripts": [{ "matches": ["https://example.com/*"],
                        "js": ["cs.js"], "run_at": "document_idle" }],
  "action": { "default_title": "Example", "default_popup": "popup.html" },
  "icons": { "16": "i16.png" },
  "cefnest": { "type": "native", "library": "plugin.so",
               "popup": { "width": 320, "height": 480 } }
}
```

- `id` = the directory name, `[a-z0-9_-]{1,64}` — it is the host part of
  every `cefnest-extension://` URI the plugin gets.
- Fields the component will never act on (`chrome_url_overrides`, `omnibox`,
  `commands`, `devtools_page`, `user_scripts`) are not silently ignored: the
  plugin stays usable and `::plugin-error` names each one as
  `unsupported-field:<name>`, which is what a row's subtitle shows when an
  extension "does nothing". `background.service_worker` is not on that list:
  it is acted on (below).
- A refused manifest (missing `name`/`version`, wrong types, an unusable
  `cefnest` block) **stays in the model** as `state=refused` with a
  machine-readable `error-message` token — "the folder is there and the list
  says nothing" is the failure mode a refusal exists to prevent. Tokens:
  `no-manifest`, `bad-id`, `bad-manifest`, `unsupported-type`,
  `duplicate-id`.

## Where plugins live

The **install root** is anchored on the session's
[data directory](data-directory.md), not on this component's name:
`<data-dir>/cefnest-plugins/`, one subdirectory per plugin. It is scanned
before every `add_search_path()` directory (the user's own install wins a
duplicate id), a missing directory is an empty list rather than an error, and
**a session without a data directory has no default plugin location at all** —
the "no directory means nothing on disk" contract holds for plugins too.
Hosts point at other directories with `add_search_path()`; packages install
with `install_async()` (a `.zip` or a `.crx` — the CRX3 shell is skipped, and
there is no signature behind it to verify, which is exactly why the only
plugins to install are ones you built).

`chrome.storage` data lands in
`<data-dir>/cefnest-plugins/<id>/storage.json` — inside the tree
`clear_data()` removes, so the persistence contract costs nothing extra.
Writes are merged on a 500 ms timer; without a data directory storage lives
in memory and dies with the process.

## What a plugin can do

| Area | Native (`.so`) | Web (content scripts + `chrome.*`) |
| --- | --- | --- |
| Context menu | `add_menu_item(label, slot, cb, user_data)` — slot 0–199, menu id 26600+slot | `chrome.contextMenus.create({title, onclick})` — the host allocates the slot |
| Session signals | `subscribe("load-end", cb, …)` — any session signal by name | — |
| Action | `set_action(widget)` — the host places it | `chrome.action.onClicked.addListener` |
| Storage | `storage_get` / `storage_set` (JSON per key) | `chrome.storage.{local,session}` — `sync` aliases local and warns once |
| Pages | — | `chrome.runtime.{id, sendMessage, getURL, onMessage}`, `chrome.tabs.{query, update}` (id/url/title/active/windowed/muted/zoom) |
| Content scripts | the manifest's `content_scripts` run the same way | same |

The two frontends can mix in one package: a native plugin may ship
`content_scripts`.

**Content script timing.** `document_start` is real: the script runs inside
the page's context before the page's own inline scripts (measured — the
page's first script can see the script's marks). The mechanism is worth
knowing for debugging: the plugin table travels to every renderer process at
spawn, and a script's bytes are fetched synchronously through
`cefnest-extension://<id>/<path>` — the same origin that serves the plugin's
pages — and evaluated. `document_end` and `document_idle` both run at load
end, from the browser process.

**The API subset and its seams**, written down so a plugin author knows what
is contract and what is today's shape:

- `chrome.runtime.sendMessage` reaches the host and the plugin's own open
  extension pages — a resident background page included; the response today
  is the host's echo of the message.
- `chrome.runtime.onMessage` listeners receive every message the plugin's
  pages send, as the parsed value (the host delivers one JSON text per
  message; the shim parses before fanning out). Several plugins on one page
  share the page-wide delivery hooks — the first shim installed owns them,
  and they fan out across every shim's listeners.
- `chrome.tabs.query` answers from the helper's browser table — id, url,
  title, active, plus `windowed`, `muted` and `zoom` read off each browser's
  host; `update` navigates, activates, and mutes. `pinned` and `discarded`
  do not exist: there is no source for them, and inventing values would be
  lying.
- A shim inside a subframe waits forever for a reply — replies are delivered
  to the main frame.

**The background page.** A manifest's `background.service_worker` names a
script; what runs is a resident **hidden browser** loading
`cefnest-extension://<id>/__cefnest_background__.html` — a synthesized page
whose one job is loading that script. The browser owns no window and no
surface: nothing appears on the nested compositor, no toplevel is spent on
it, it never becomes the active browser, and it is absent from `tabs.query`.
It is hidden in Chromium's own sense (`WasHidden`), so rendering settles to
the hidden-tab schedule while timers, network and storage keep running. Its
`chrome.*` is the same shim any extension page gets, so `onMessage` and
`chrome.storage` work there; messages from the plugin's content scripts
reach it through `chrome.runtime.sendMessage`.

What this is **not**: a Chrome service worker. There is no SW lifecycle —
no wake-on-event, no suspend, no install/activate phases. The page exists
for exactly as long as the plugin is enabled: enabling creates it, disabling
closes it, a helper restart rebuilds it. Events a plugin could receive
through Chrome's SW (alarms, webRequest, tabs triggers) do not exist here.
The manifest field is promoted because the *shape* — a script the plugin
author wrote, always running while enabled — is honest now; the semantics
stay Chrome's own, and this page is where that boundary is written.

**Payload ceiling: 1 MiB.** The shim refuses to send more, the browser
process re-checks on receipt, and the respond path refuses on send —
`plugin-payload-too-large`, by name, on the page and on the host. A plugin's
oversized message must never take the control channel down; the channel
stays alive and a small call right after still works (measured in the gate).

## What a plugin can never do here

- **Content scripts have no isolated world.** CEF has no such primitive
  (checked against the CEF tree, not assumed). A content script shares the
  page's one JS global: the page can read it, overwrite it, and call the
  bridge it installed. Content scripts therefore hold no privileges — every
  privileged call goes back to the host and is judged against the permission
  table. Even so, a page can *pretend to be* the plugin by calling the same
  bridge with the plugin's id; that boundary is the architecture's, and this
  document does not pretend it away. Two plugins on one page each install
  their shim; the last `window.chrome` wins.
- **A native plugin owns the host process.** No sandbox, no capability
  filtering: a `.so` runs with everything the host can do. Default policy:
  only the data directory's install root is scanned; anything else needs the
  host to point at it. Install only plugins you built.
- **No MV3 service worker *semantics*.** `background.service_worker` runs as
  a resident hidden page (above) — no SW lifecycle, no wake-on-event. A
  manifest carrying any other background shape (`page`, `scripts`) is named
  as `unsupported-field:background`.
- **No signature scheme, no store, no auto-update.** `install_async` skips
  the CRX3 header because there is nothing behind it to verify.

## Permissions

The vocabulary is WebExtension's (`storage`, `tabs`, `contextMenus`,
`activeTab`, …); one table serves both frontends. `enable()` on a plugin
whose declared permissions are not all granted emits `::grant-request`
first, then reads the table: a host that grants before the emission returns
completes the enable in the same call; a host that shows a dialog gets
`FALSE` now and re-enables later. **An unanswered request is a deny** — the
default the table exists to keep. The built-in handlers enforce the table
themselves: a plugin that never declared `storage` calling
`chrome.storage.local.get` is refused by name
(`permission-denied:storage` on `::plugin-error`, an error to the page), and
the other plugins keep working.

## Unloading

`disable()` undoes everything from the host's own ledger: the menu rows go
off the helper's table, the signal connections disconnect, the widget
reference drops, the `.so` unmaps — a plugin registers callbacks and never
GTypes, so nothing it did can outlive it. `::closed` (the helper died) runs
the same unload before any signal could reach a handler whose code is
already unmapped. A restart re-loads what is still enabled and re-announces
its rows; the gate runs disable → stop → start → enable → stop → start and
counts every load and unload under `G_DEBUG=fatal-criticals`.
