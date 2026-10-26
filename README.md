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
  JavaScript dialogs and file pickers, page ↔ host messages, clipboard and
  drag & drop across the window edge
- Zoom, mute, loading progress, page fullscreen, accessibility (ORCA-ready
  AT-SPI), frame count
- The context menu's View Source is the component's own read-only window:
  line numbers, syntax highlighting, find, wrap, copy — the source stays in
  memory, no temp file, no external editor

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
| GtkSourceView | 5.4+ (View Source window) |
| meson / ninja / cmake | meson 1.3+, cmake 3.16+ |
| CEF (`cef_binary_*_linux64_minimal`) | verified with stable 156.0.14 |

`wayland-scanner` must be on `PATH`. Rendering needs working unprivileged user
namespaces (Chromium's sandbox); see
[docs/troubleshooting.md](docs/troubleshooting.md) if your host restricts them.

## Build

```bash
./scripts/fetch-cef.sh                # 1. fetch the verified CEF binary distribution
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release   # 2. the Chromium side
cmake --build helper/build -j
meson setup builddir && ninja -C builddir                    # 3. widget, demo, tests
```

Stage 1 downloads ~316 MB (about 1.5 GB unpacked). Skip it and everything
still builds — the demo then drives a protocol-level test client instead of
Chromium.

## Install

```bash
meson setup builddir -Dprefix=$HOME/.local -Dinstall-helper=true
ninja -C builddir install
```

| Option | Default | What it decides |
| --- | --- | --- |
| `install-helper` | `false` | also install the helper + the CEF runtime (~1.5 GB) |
| `install-demo` | `true` | install `cefnest-demo` and the fallback test client |

The release pipeline (`.cnb.yml`, version tags only) packs two packages, each
as a deb and an rpm: `cefnest` is the library + helper + CEF runtime,
`cefnest-dev` (deb) / `cefnest-devel` (rpm) adds headers and `cefnest.pc`. A
host builds with `pkg-config --cflags --libs cefnest`. The packaged CEF
runtime ships stripped — ~356 MB installed, ~126 MB packed; the plain
`meson install` above copies it unstripped, at ~1.5 GB.

The library finds its helper without requiring any environment variables:
`CEFNEST_HELPER` or `CHROMIUM_HELPER` (must contain `/`), else the path
compiled in from `prefix + libexecdir + <arch>`, else the development tree.
See `docs/control-channel.md` for what the helper process itself is given.

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
menus, permissions, error handling, a frame counter, a User-Agent switcher, a
persistent data directory, the DevTools switch.
What each one shows is listed in [examples/README.md](examples/README.md).

## Data directory

**A session keeps nothing on disk unless you name a directory.** With no data
directory the helper runs as Chromium's incognito: cookies, localStorage,
IndexedDB and the HTTP cache live in memory, the one install-level directory
CEF insists on writing is a mode-0700 temporary directory under
`$XDG_RUNTIME_DIR`, and nothing is ever written under `~/.cache`. To keep page
data across restarts, name one absolute path before `start()`:

```c
chromium_session_set_data_directory (session, "/home/me/.local/share/myapp/cefnest");
chromium_session_start (session, "https://example.com", &error);
```

The full contract — the rules, where to put the directory and what an
uninstall does with it per packaging format, the standalone helper's
environment variable, and cleaning up the pre-156 `~/.cache` directories — is
in [docs/data-directory.md](docs/data-directory.md).

## Developer tools

The inspector is off by default and has no other way in: while
`devtools-enabled` is FALSE the helper never calls `ShowDevTools`, the context
menu has no 检查 row, and F12 / Ctrl+Shift+I do nothing. Turn it on before
`start()` — or at any time while a helper runs:

```c
chromium_session_set_devtools_enabled (session, TRUE);
```

The inspector opens in a window of its own, as a separate browser associated
with the page, and reaches the host as `::browser-created` with `devtools` TRUE;
show it like any standalone browser (`examples/devtools.c` does). Closing the
inspector leaves the page running, and closing the page takes the inspector with
it. Enabling the switch gives whoever is at the keyboard full debugging power
over every page the session loads — arbitrary script in the page, and read
access to its storage and cookies — so it belongs behind a user's own choice,
not on by default. The demo exposes the same switch as `--devtools`.

## API

`ChromiumView` is the widget; `ChromiumSession` is the control plane that
starts the helper and tracks the page. Full per-call contracts are documented
inline in `src/chromium-session.h`; the method and signal digest — every call
group and what each signal carries — is in [docs/api.md](docs/api.md).

Plugins: a directory with a `cefnest-plugin.json` manifest becomes a row in a
`GListModel` (via `chromium_session_get_plugin_manager()`), and enabling one
reaches the running helper — its resources are served on `cefnest-extension://`,
content scripts run at `document_start` on top of a `chrome.*` subset, and a
native `.so` plugin registers context-menu rows and signal handlers through a
five-verb registry, with everything it registered undone on disable. The
manifest is shape-compatible with `manifest_version: 3`; MV3 service workers
are not. The install root is the data directory's own `cefnest-plugins/` — no
data directory, no default plugin location (explicit `add_search_path()` still
works). Content scripts share the page's JS global, so they hold no
privileges. See [docs/plugins.md](docs/plugins.md) and
[Plugins in docs/api.md](docs/api.md#plugins).

## Demo environment variables

| Variable | What it does |
| --- | --- |
| `CHROMIUM_URL` | page to open |
| `CEFNEST_HELPER` | helper path (must contain `/`); if it does not exist, the demo falls back to the test client |
| `CHROMIUM_HELPER` | set to anything forces session mode; a value with `/` names the helper and is used as written, even when it does not exist |
| `CHROMIUM_SESSION_SCRIPT` | script file, one command per line (`load`, `js`, `find`, `menu-pick`, `download-accept`, …) |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | write a PNG of the widget after N ms and exit |
| `CHROMIUM_HELPER_NO_SANDBOX` | `1` runs the helper without Chromium's sandbox |
| `CHROMIUM_USER_AGENT` | default User-Agent baked in at helper startup |
| `G_MESSAGES_DEBUG` | **space**-separated log domains: `all`, or `nested chromium-view chromium-session` |

The demo also accepts `--data-dir=<absolute path>` (see
[docs/data-directory.md](docs/data-directory.md)) and `--devtools` (see
[Developer tools](#developer-tools)), and reads extra knobs (`CEFNEST_DOWNLOAD_*`, `CEFNEST_PERMISSION`,
`CEFNEST_DRAG_OUT`, `CHROMIUM_WINDOW`, `CHROMIUM_FRAME_METRICS`, the clipboard
seed/watch variables, `CHROMIUM_A11Y`); see `src/main.c`.

## Troubleshooting

The symptom → fix table — sandbox refusals, a missing helper, a blank page
area, log domains that print nothing, teardown order — is in
[docs/troubleshooting.md](docs/troubleshooting.md).

## Known limitations

- One seat, one pointer, no touch; no pinch zoom or tablet input.
- A drag started in an independent window leaves from the wrong surface.
- `::popup-requested` carries only the URL.
- H.264/MP4 is absent from this CEF distribution; VP9 plays.
- A failed navigation reports twice, and `::load-end` still fires.
- Only the renderer's death is reported, and only how it died.
- `complete` on a download is not the moment the file appears.
- Find walks matches one at a time.
- Plugin context-menu rows come in one shape: a whole row added and removed,
  not Chrome's per-page graying; a native `.so` plugin runs in the host
  process with everything the host can do (see "Plugins" below).
- A permission can be granted or denied, not narrowed; an unanswered one also
  costs the page its next ask.
- Page audio bypasses the widget; `set_muted()` is the only switch.
- The renderer is sandboxed; the helper itself is not, and cannot be.
- View Source shows the **serialized DOM** — script changes included — not
  the network's original bytes.
- Content scripts share the page's one JS global (CEF has no isolated
  world): the page can read and overwrite the bridge a plugin hangs off the
  global, so content scripts hold no privileges — privileged calls go back to
  the host, which rules them against the permission table.
- The API is not stable — link against a build you built yourself.

## Documentation

- `docs/data-directory.md` — the data-directory contract: nothing on disk by
  default, one path for a persistent profile, exclusivity, uninstall
  behaviour, legacy `~/.cache` cleanup.
- `docs/api.md` — the method and signal digest; the authoritative contract is
  `src/chromium-session.h`.
- `docs/troubleshooting.md` — symptom → fix, from `No usable sandbox!` to log
  domains.
- `docs/plugins.md` — the plugin contract: the manifest, where plugins live,
  what each frontend can do, the `chrome.*` subset and its seams, and the
  isolation facts.
- `docs/control-channel.md` — the NDJSON protocol between session and helper
  (commands, events, the four interaction shapes). Only needed for custom
  helpers or direct channel work.
- `docs/cef-features.zh-CN.md` — (简体中文) the CEF 156 interface-by-interface
  status table: every handler, what is implemented, what is not, what is out
  of scope, and the consequence of each gap.
- `site/` — the project page (Vite + Vue + Bootstrap); `npm run dev` and
  `npm run build` inside it.

## License

LGPL-2.1-only — see `LICENSE`. The helper links CEF (BSD-3-Clause with its own
third-party notices), which is not redistributed by this repository.
