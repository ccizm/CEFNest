# CEFNest

English | [简体中文](README.zh-CN.md)

A GTK 4 widget that renders web pages with **real Chromium** (CEF), composited
natively into the widget by a Wayland compositor running in the same process.
No XWayland, no offscreen rendering: Chromium draws onto its own Wayland
surface, and the widget paints it like any other content.

## Features

- Native Wayland only — Chromium's own `ozone/wayland` backend, GPU or shm
- One helper process serves many tabs and standalone windows
- Host-answerable APIs: downloads, find, context menu, permission prompts,
  page ↔ host messages, clipboard and drag & drop across the window edge
- Zoom, mute, accessibility (ORCA-ready AT-SPI), frame metrics

## How it works

```
 GTK 4 application
   └── ChromiumView (a GtkWidget)
         └── NestedCompositor            src/nested*.c — a libwayland-server compositor
               · its own socket in $XDG_RUNTIME_DIR
               · receives shm / dmabuf buffers, composites the surface stack
               · forwards the widget's real pointer, keyboard and IM to the client
         ▲
         │ Wayland protocol (the client side of the socket)
         │
 cefnest-helper                          helper/ — a CEF application, separate process
   · started with WAYLAND_DISPLAY=<the nested compositor's socket>
   · Chromium renders the page into that compositor
   · control plane: NDJSON over a unix socket, both directions
```

| Piece | File | Responsibility |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | the compositor: globals, surfaces, damage, input, clipboard, drag in both directions across the window edge |
| `ChromiumView` | `src/chromium-view.c` | the widget: hands its GdkSurface's input to the compositor, paints the composition |
| `ChromiumSession` | `src/chromium-session.c` | the control plane: starts and stops the helper, tracks page state, runs JavaScript, brokers downloads, find, the page's context menu and its permission requests |

## Requirements

| Package | Minimum |
| --- | --- |
| GTK 4 | 4.14 |
| Wayland (server + client) | 1.21+ (`axis_value120`) |
| wayland-protocols | with `cursor-shape`, `fractional-scale`, `text-input` |
| xkbcommon / json-glib / gbm + libdrm | any recent |
| meson / ninja / cmake | meson 1.3+, cmake 3.16+ |
| CEF (`cef_binary_*_linux64_minimal`) | verified with stable 154.0.34 |

`wayland-scanner` must be on `PATH`. Rendering needs working unprivileged user
namespaces (Chromium's sandbox); see **Troubleshooting** if your host restricts
them.

## Build

```bash
./scripts/fetch-cef.sh                # 1. fetch the verified CEF binary distribution
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release   # 2. the Chromium side
cmake --build helper/build -j
meson setup builddir && ninja -C builddir                    # 3. widget, demo, tests
```

Stage 1 downloads ~500 MB. Skip it and everything still builds — the demo then
drives a protocol-level test client instead of Chromium.

## Install

```bash
meson setup builddir -Dprefix=$HOME/.local -Dinstall-helper=true
ninja -C builddir install
```

| Option | Default | What it decides |
| --- | --- | --- |
| `install-helper` | `false` | also install the helper + the CEF runtime (~1.5 GB) |
| `install-demo` | `true` | install `cefnest-demo` |

The release pipeline (`.cnb.yml`, version tags only) packs two packages, deb
and rpm: `cefnest` is the library + helper + CEF runtime, `cefnest-dev` adds
headers and `cefnest.pc`. A host builds with
`pkg-config --cflags --libs cefnest`.

The library finds its helper without any environment: `CEFNEST_HELPER` or
`CHROMIUM_HELPER` (must contain `/`), else the path compiled in from
`prefix + libexecdir + <arch>`, else the development tree. See
`docs/control-channel.md` for what the helper process itself is given.

## Quick start

Run the demo:

```bash
CHROMIUM_URL=https://example.com ./builddir/src/cefnest-demo
```

Embed in your app — the whole API is three objects (`examples/minimal.c` is
exactly this, in working form):

```c
GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

gtk_window_set_child (GTK_WINDOW (window), view);          // 1. view into a window
chromium_session_set_view (session, CHROMIUM_VIEW (view)); // 2. view into the session
chromium_session_start (session, "https://example.com", &error); // 3. start
```

The order matters: the view must be in a window before the session starts, so
the helper gets a compositor with a live surface. On window close, call
`chromium_session_stop()` (a `close-request` handler is the usual place).

More samples in [`examples/`](examples/README.md) — one focused program per
task: a toolbar, tabs, standalone windows, scripting, a find bar, downloads,
menus, permissions, error handling, a frame counter, a User-Agent switcher.
What each one shows is listed in [examples/README.md](examples/README.md).

## API

`ChromiumView` is the widget; `ChromiumSession` is the control plane that
starts the helper and tracks the page. Full per-call contracts are documented
inline in `src/chromium-session.h`.

**Methods**

| Group | Calls |
| --- | --- |
| Lifecycle | `chromium_session_new()` · `set_view()` · `start()` · `stop()` |
| Navigation | `load_uri()` · `go_back()` · `go_forward()` · `reload()` · `stop_loading()` |
| State | `get_uri()` · `get_title()` · `get_loading()` · `get_can_go_back()` · `get_can_go_forward()` · `get_running()` · `get_muted()` · `get_zoom_level()` · `get_frame_count()` |
| JavaScript | `run_javascript()` · `run_javascript_expression()` → answer on `::script-result` |
| Downloads | answer `::download-begin` with `accept_download()` / `reject_download()`; in flight: `pause_download()` / `resume_download()` |
| Find | `find()` · `stop_finding()` → answers on `::find-result` |
| Context menu | answer `::context-menu` with `context_menu_select()` / `context_menu_dismiss()` |
| Permissions | answer `::permission-request` with `permission_respond()` |
| Tabs / windows | `new_browser()` · `select_browser()` · `close_browser()` |
| Page / misc | `set_muted()` · `zoom("in"/"out"/"reset")` · `set_accessibility()` |

**Signals**

| Signal | Carries | Notes |
| --- | --- | --- |
| `::changed` | — | uri / title / loading / back-forward state moved |
| `::ready` | — | helper handshake done |
| `::load-end` | `url, http_code` | fires on failure too, with code 0 |
| `::load-error` | `error_code, url, message` | a navigation that never committed |
| `::renderer-exited` | `status, error_code, message` | the page's render process died |
| `::script-result` | `id, ok, value, message` | answer to a `run_javascript` call |
| `::download-begin` | `id, url, suggested_name, mime_type, total_bytes` | must be answered |
| `::download-progress` / `::download-end` | see header | progress per whole percent |
| `::find-result` | `identifier, count, active_ordinal, final` | one request may answer more than once |
| `::context-menu` | `x, y, items` | return TRUE to draw your own menu, then answer |
| `::permission-request` | `id, kind, origin, permissions` | return TRUE to own the answer; default is deny |
| `::permission-dismissed` | `id, result` | a held prompt was replaced by the page |
| `::browser-created` / `::browser-selected` / `::browser-closed` | `id, …` | tab and window lifecycle |
| `::popup-requested` | `url` | target=_blank; foreground ones already navigated in place |
| `::page-message` | `json` | page called `cefnestPostMessage()`; reply via `run_javascript` |
| `::primary-selection` / `::copy-text` / `::open-url` | `text` / `text` / `url` | selection bridge, menu copy, menu "open in new window" |
| `::zoom-changed` | `level` | 0.0 = 100 %; also after each main-frame load |
| `::closed` | `reason` | helper gone for good; `start()` again for a new page |
| `::helper-exited` | — | the process has been reaped; destroy the display here |

## Demo environment variables

| Variable | What it does |
| --- | --- |
| `CHROMIUM_URL` | page to open |
| `CEFNEST_HELPER` / `CHROMIUM_HELPER` | force a helper path (must contain `/`); a nonexistent one makes the demo fall back to the test client |
| `CHROMIUM_SESSION_SCRIPT` | script file, one command per line (`load`, `js`, `find`, `menu-pick`, `download-accept`, …) |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | write a PNG of the widget after N ms and exit |
| `CHROMIUM_HELPER_NO_SANDBOX` | `1` runs the helper without Chromium's sandbox |
| `CHROMIUM_USER_AGENT` | default User-Agent baked in at helper startup |
| `G_MESSAGES_DEBUG` | space-separated log domains: `all`, or `nested chromium-view chromium-session` |

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `No usable sandbox!` at helper start | the host restricts unprivileged user namespaces. Install an AppArmor profile allowing userns for `cefnest-helper` (recommended), or set `CHROMIUM_HELPER_NO_SANDBOX=1` |
| `start()` says `no helper binary at …` | that exact path is what was tried: install `cefnest-helper`, or point `CEFNEST_HELPER` at an absolute path; in a dev tree, run cmake first |
| Window shows a blank page area | confirm the helper is running and the session is a native Wayland session (`GDK_BACKEND=wayland`); X11 does not render |
| `G_MESSAGES_DEBUG` prints nothing | the domain list is **space**-separated; a comma list matches nothing |
| Closing the last tab closes the page | one session is one helper; open a new browser first, or handle `::closed` |
| `Lost connection to Wayland compositor` before `::helper-exited` | normal teardown order — destroy your display only after `::helper-exited` |

## Known limitations

- One seat, one pointer, no touch; no pinch zoom or tablet input.
- A drag started in an independent window leaves from the wrong surface.
- `::popup-requested` carries only the URL.
- H.264/MP4 is absent from this CEF distribution; VP9 plays.
- A failed navigation reports twice, and `::load-end` still fires.
- Only the renderer's death is reported, and only how it died.
- `complete` on a download is not the moment the file appears.
- Find walks matches one at a time.
- The helper extends the context menu; the host cannot add its own rows.
- A permission can be granted or denied, not narrowed; an unanswered one also
  costs the page its next ask.
- Page audio bypasses the widget; `set_muted()` is the only switch.
- The renderer is sandboxed; the helper itself is not, and cannot be.
- The API is not stable — link against a build you built yourself.

## Documentation

- `docs/control-channel.md` — the NDJSON protocol between session and helper
  (commands, events, the four interaction shapes). Only needed for custom
  helpers or direct channel work.

## License

LGPL-2.1-only — see `LICENSE`. The helper links CEF (BSD-3-Clause with its own
third-party notices), which is not redistributed by this repository.
