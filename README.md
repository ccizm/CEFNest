# ChromiumGTK

A GTK 4 widget that shows a web page rendered by **real Chromium**, composited
into the widget through a Wayland compositor that runs inside the same process.

Two constraints decide everything here:

- **Native Wayland.** No XWayland, no X11 client, no `XEMBED`. The page is drawn
  by Chromium's own `ozone/wayland` backend.
- **No OSR.** There is no "offscreen rendering" buffer being copied into a Cairo
  surface. Chromium creates a Wayland surface; our compositor receives it the
  way any compositor would, and paints it as a `GdkTexture` inside the widget.

That rules out every "embed a browser in GTK" approach that does not ship a
compositor, so this repository ships one.

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
 chromium-helper                          helper/ — a CEF application, separate process
   · started with WAYLAND_DISPLAY=<the nested compositor's socket>
   · Chromium renders the page into that compositor
   · control plane: NDJSON over a unix socket, both directions
```

Three pieces, and each only does one job:

| Piece | File | Responsibility |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | the compositor: globals, surfaces, damage, input, clipboard |
| `ChromiumView` | `src/chromium-view.c` | the widget: hands its GdkSurface's input to the compositor, paints the composition |
| `ChromiumSession` | `src/chromium-session.c` | the control plane: starts and stops the helper, tracks page state, runs JavaScript |

Globals the compositor advertises, at the versions in `src/nested-internal.h`:
`wl_compositor`, `wl_shm`, `wl_seat` (8, so pointer and keyboard can carry
`axis_value120`), `wl_output`, `wl_data_device_manager` (3), `xdg_wm_base`,
`zwp_linux_dmabuf_v1` (5, with feedback), `wp_viewporter`,
`wp_fractional_scale_manager_v1`, `zwp_text_input_manager_v3`,
`wp_cursor_shape_manager_v1`.

## Requirements

Measured on the development machine (Debian 13, `linux 6.12`):

| Package | Version used | Minimum |
| --- | --- | --- |
| gtk4 | 4.18.6 | 4.14 |
| wayland (server + client) | 1.23.1 | any with `axis_value120` (1.21+) |
| wayland-protocols | 1.44 | must ship `staging/cursor-shape`, `staging/fractional-scale`, `unstable/text-input` |
| xkbcommon | 1.7.0 | — |
| gbm / libdrm | 25.0.7 / 2.4.124 | — |
| json-glib | 1.10.6 | — |
| meson / ninja / cmake | 1.x / 1.12 / 3.16+ | — |
| CEF (`cef_binary_*_linux64_minimal`) | 154.0.34+g14c5a08+chromium-154.0.8037.98 | any that builds against its own CMake helpers |

`wayland-scanner` must be on `PATH` (it generates the protocol code at build
time — nothing is checked in).

## Build

Two stages, and the first is optional.

```bash
# 1. the Chromium side — needs a CEF binary distribution
cmake -S helper -B helper/build \
      -DCEF_ROOT=/path/to/cef_binary_154.0.34+g14c5a08+chromium-154.0.8037.98_linux64_minimal \
      -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build --config Release -j

# 2. the widget, the demo, the tests
meson setup builddir
ninja -C builddir
```

Skip stage 1 and everything still builds: the demo then drives
`test/shm-client` instead of Chromium, which is enough to exercise the
compositor, the seat, the clipboard bridge and the popups.

## Run

```bash
# no Chromium involved: the nested compositor embeds test/shm-client
./builddir/src/chromium-demo

# the real thing
CHROMIUM_HELPER=$PWD/helper/build/Release/chromium-helper \
CHROMIUM_URL=https://example.com \
./builddir/src/chromium-demo
```

The demo reads its whole configuration from the environment, because the point
of it is reproducible runs:

| Variable | What it does |
| --- | --- |
| `CHROMIUM_HELPER` | set (any value containing `/`) to enter session mode; the default is the path compiled in `src/meson.build` |
| `CHROMIUM_URL` | page to open |
| `CHROMIUM_SESSION_SCRIPT` | file of one command per line: `load`, `back`, `forward`, `reload`, `reload-hard`, `stop`, `js`, `wait`, `shot`, `dump`, `hover`, `click`, `drag`, `wheel`, `close`, `quit` — see `test/*.session` |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | grab the widget after N ms and write a PNG, then exit |
| `CHROMIUM_WINDOW` | `fullscreen` or `maximize`, for deterministic geometry |
| `CHROMIUM_CLIPSET` / `CHROMIUM_CLIPWATCH` | seed the host clipboard, and bridge it into the page |
| `G_MESSAGES_DEBUG` | **space-separated** domains: `all`, or `nested chromium-view chromium-session` — a comma list matches nothing |

## Test

```bash
meson test -C builddir                       # 9 tests
meson test -C builddir --suite selftest      # the eight seat selftests
meson test -C builddir --suite protocol      # the implementation-slot audit
```

`seat-selftest` starts the nested compositor, launches `shm-client` against it,
runs the scripted pointer/keyboard/text-input sequence and prints what the
client saw. Each mode asks for a different set of protocol objects:

```bash
./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --popup
```

with `--dmabuf`, `--text-input`, `--cursor-shape`, `--popup`, `--refused`,
`--dnd-requests`, `--gtk-client`. On a machine with no display they report
`skipping` and exit 0.

`--gtk-client` is not another flavour of `shm-client`: it finishes the run by
launching a second copy of `seat-selftest` as a GTK client of its own
compositor, and fails if that client is killed by an assertion of its own.
A raw client only sets a flag when an event arrives, so it cannot crash on a
legal event sequence — a toolkit can, and did
(`docs/clipboard-offer-during-client-init.md`).

The raw client does check one thing of its own, and the run stops on it: a
`wl_pointer.axis_stop` may not share a `wl_pointer.frame` with the `axis` event
it stops, which is the shape that makes Chromium throw the scroll delta away
(`docs/wheel-resolution-and-key-repeat.md`). `seat-selftest` waits for its
child rather than polling it, so a client that leaves by signal or with a
non-zero status fails the test at that step instead of printing the rest of the
script to nobody.

What the compositor sends is only half of it, so the wheel is also checked where
it matters:

```bash
GDK_BACKEND=wayland CHROMIUM_HELPER=$PWD/helper/build/Release/chromium-helper \
  CHROMIUM_URL=file://$PWD/test/wheel-page.html \
  CHROMIUM_SESSION_SCRIPT=$PWD/test/wheel.session ./builddir/src/chromium-demo
```

`test/wheel.session` reads `document.scrollingElement.scrollTop` after each
`wheel` step, and expects 120 CSS px per detent — 360 for `wheel 0 3`, 420 after
the half detent that follows it.

`test/audit-impl-slots.py` is the static half of the same idea: it reads the
generated protocol headers and every `wl_*_interface` initializer in `src/`, and
fails if a request slot a client can reach is left NULL — an empty slot is not a
refusal, it aborts the compositor (`docs/empty-request-slot.md`).

`test/uinput-inject.c` replays evdev events through `/dev/uinput` to drive the
widget itself, which needs write access to the node (`sudo`).

## Using it as a library

```c
GtkWidget        *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession  *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
if (!chromium_session_start (session, "https://example.com", &error))
  g_printerr ("%s\n", error->message);
```

`ChromiumSession` signals: `::changed` (uri / title / loading / history moved),
`::ready` (the helper answered its handshake), `::load-end` (uri, HTTP code),
`::script-result` (id, ok, JSON value, message), `::closed` (the helper is gone
and will not come back).

## Where the detail is

`docs/` holds the write-ups — Chinese, with the measured before/after evidence.
They are the record of *why* the code is shaped like this:

| Document | About |
| --- | --- |
| `nested-split.md` | how `nested.c` became per-protocol modules |
| `frame-clock-pacing.md` | painting on the GTK frame clock instead of a timer |
| `popup-compositing.md` | the surface stack, placement and dismissal of page popups |
| `clipboard-bridge.md` | `wl_data_device` ↔ `GdkClipboard` in both directions |
| `session-lifecycle.md` | starting, stopping and dying with the helper |
| `resource-limits.md` | shm pool caps, allocation checks, the dmabuf fd leak |
| `empty-request-slot.md` | a NULL request slot killing the whole desktop |
| `wheel-resolution-and-key-repeat.md` | `axis_value120`, one frame per gesture, auto-repeat bookkeeping |
| `clipboard-offer-during-client-init.md` | a legal `data_offer` sent one dispatch too early kills a GTK client |

## Known limitations

- **No drag and drop out of the page.** `start_drag` is refused on purpose: a
  surface inside a widget has nowhere to be dropped. The version 3 requests are
  answered, they just do nothing.
- **One seat, one pointer, no touch.** The embedding widget is the only device
  the compositor knows about.
- **Load failures are not a signal yet.** `load-error` arrives on the channel and
  is logged; there is no `ChromiumSession::load-error` to connect to.
- **The helper's sandbox policy is undecided**, so treat the process as
  unsandboxed.
- **Native Wayland only, by design.** Under an X11 session GDK cannot give the
  widget a Wayland surface to hand to clients, so the component will not work
  there.
- The API is not stable: nothing here is installed, and `src/meson.build` builds
  a static library.

## License

LGPL-2.1-only — see `LICENSE`. The Chromium helper links CEF, which is BSD-3-Clause with
its own third-party notices; those are not part of this repository and are not
redistributed by it.
