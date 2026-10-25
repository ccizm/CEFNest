# examples/

English | [简体中文](README.zh-CN.md)

Each example demonstrates one thing and is a complete, runnable host on its
own. After a build they live in `builddir/examples/cefnest-<name>`; they are
not installed by `meson install`.

```bash
ninja -C builddir
CEFNEST_URL=https://example.com ./builddir/examples/cefnest-tabs
```

How a helper is found: **Install** in the root `README.md`. The channel
protocol between session and helper: [`docs/control-channel.md`](../docs/control-channel.md).

| Example | What it shows |
| --- | --- |
| `minimal.c` | the minimal host: window + `ChromiumView` + `ChromiumSession`, ~56 lines |
| `host.c` | the four signals a host has to answer (download, permission, JS, context menu — the menu is deliberately left unconnected); script-driven via `CEFNEST_HOST_SCRIPT`, tested without a keyboard |
| `browser.c` | a single-page browser shell: address entry, back/forward, zoom, mute — all riding on `::changed` and the getters, no per-event bookkeeping |
| `tabs.c` | multi-tab: several browsers in one helper, and how `::browser-created` / `::browser-selected` drive a tab row |
| `windows.c` | windowed browsers: `chromium_view_new_shared()` gives each CefWindow a real GTK window |
| `script.c` | both ways of running JavaScript and `::script-result`; the `cefnestPostMessage()` / `::page-message` round trip |
| `findbar.c` | in-page search: how `find()` and `::find-result`'s incremental answers become a find bar |
| `downloads.c` | downloads: `::download-begin` must be answered, one progress row per download, the three endings of `::download-end` |
| `menus.c` | the context menu: draw your own popover and answer after returning TRUE; the session's default menu when you don't |
| `permissions.c` | permission prompts: `::permission-request` as allow/deny rows, and the cost of not answering |
| `errors.c` | the three "the page is not what it was" signals: `::load-error`, `::renderer-exited`, `::closed` — and how to wire each |
| `observe.c` | the frame counter and the accessibility switch: making "the page froze" measurable |
| `devtools.c` | the inspector switch: F12 / the 检查 row, and a GTK window per inspector it opens |
| `useragent.c` | switching the User-Agent at runtime: restart the session, the new `CHROMIUM_USER_AGENT` rides with the helper |
| `persistent.c` | a data directory: localStorage read back across restarts, and `clear_data()` when the user asks to forget |

Each file's header comment is its full write-up; start with `minimal.c` and
pick what you need.
