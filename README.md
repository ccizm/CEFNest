# CEFNest

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
 cefnest-helper                          helper/ — a CEF application, separate process
   · started with WAYLAND_DISPLAY=<the nested compositor's socket>
   · Chromium renders the page into that compositor
   · control plane: NDJSON over a unix socket, both directions
```

Three pieces, and each only does one job:

| Piece | File | Responsibility |
| --- | --- | --- |
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | the compositor: globals, surfaces, damage, input, clipboard |
| `ChromiumView` | `src/chromium-view.c` | the widget: hands its GdkSurface's input to the compositor, paints the composition |
| `ChromiumSession` | `src/chromium-session.c` | the control plane: starts and stops the helper, tracks page state, runs JavaScript, brokers downloads and find |

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
# 1. fetch the pinned CEF binary distribution (gitignored, ~500 MB unpacked)
./scripts/fetch-cef.sh

# 2. the Chromium side — picks up third_party/cef/dist automatically
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. the widget, the demo, the tests
meson setup builddir
ninja -C builddir
```

`scripts/fetch-cef.sh` pins the exact CEF build; a different distribution can
be passed with `-DCEF_ROOT=…`. Bumping the pinned version means re-running the
full gate — a Chromium milestone also changes what Wayland protocols the
client asks for (docs/protocol-ceilings.md is the re-measurement record).

Skip stage 1 and everything still builds: the demo then drives
`test/shm-client` instead of Chromium, which is enough to exercise the
compositor, the seat, the clipboard bridge and the popups.

## Run

```bash
# no Chromium involved: the nested compositor embeds test/shm-client
./builddir/src/cefnest-demo

# the real thing
CHROMIUM_HELPER=$PWD/helper/build/Release/cefnest-helper \
CHROMIUM_URL=https://example.com \
./builddir/src/cefnest-demo
```

The demo reads its whole configuration from the environment, because the point
of it is reproducible runs:

| Variable | What it does |
| --- | --- |
| `CHROMIUM_HELPER` | set (any value containing `/`) to enter session mode; the default is the path compiled in `src/meson.build` |
| `CHROMIUM_URL` | page to open |
| `CHROMIUM_SESSION_SCRIPT` | file of one command per line: `load`, `back`, `forward`, `reload`, `reload-hard`, `stop`, `js`, `wait`, `shot`, `dump`, `hover`, `click`, `drag`, `wheel`, `download-accept <path>`, `download-reject`, `find <pattern>`, `find-case <pattern>`, `find-next`, `find-prev`, `find-stop`, `close`, `quit` — see `test/*.session` |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | grab the widget after N ms and write a PNG, then exit |
| `CHROMIUM_WINDOW` | `fullscreen` or `maximize`, for deterministic geometry |
| `CHROMIUM_CLIPSET` / `CHROMIUM_CLIPWATCH` | seed the host clipboard, and bridge it into the page |
| `CHROMIUM_HELPER_NO_SANDBOX` | set to exactly `1` to run the helper without Chromium's sandbox; anything else — including a typo — leaves it on (`docs/sandbox-policy.md`) |
| `CEFNEST_DOWNLOAD_DIR` | where the demo panel auto-accepts a download; defaults to the XDG download directory (`docs/download-path.md`) |
| `CEFNEST_DOWNLOAD_ASK` | any value: every download waits for the 保存/丢弃 buttons or a script step instead of landing on disk |
| `G_MESSAGES_DEBUG` | **space-separated** domains: `all`, or `nested chromium-view chromium-session` — a comma list matches nothing |

## Test

```bash
meson test -C builddir                       # 10 tests
meson test -C builddir --suite selftest      # the nine seat selftests
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

`--dmabuf` is scored on the compositor's side of the import: the client falls
back to `wl_shm` on any refusal and still exits 0, so the run counts what the
compositor actually imported from its own log and fails if a buffer was refused
or none arrived while dmabuf was advertised. A machine with no usable render
node says so — that run is fallback evidence, not import evidence.

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
GDK_BACKEND=wayland CHROMIUM_HELPER=$PWD/helper/build/Release/cefnest-helper \
  CHROMIUM_URL=file://$PWD/test/wheel-page.html \
  CHROMIUM_SESSION_SCRIPT=$PWD/test/wheel.session ./builddir/src/cefnest-demo
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

`test/download-gate.sh` is the download half, and it stays a documented manual
gate because it opens real windows: ten runs against `test/fake-helper.py` and
then against real Chromium, 41 assertions over the whole matrix — a file that
lands, a name deduplicated, `../` in a suggested name that cannot escape the
target directory, a refusal that leaves the directory empty, a mid-flight
cancel, and a host that never answers at all. The real-Chromium cancel runs
against a local server that trickles a 2 MiB body at 32 KiB per 100 ms: served
in one write, a localhost download finishes before the script gets a chance to
reject it, so the assertion would pass on a race instead of on the cancel path.
`docs/download-path.md` §6 has the matrix.

`test/find-gate.sh` is the find half, same manual-gate convention. Six runs —
four against `test/fake-helper.py`, two against real Chromium on
`test/find-page.html`, 34 assertions — and the point of the fake runs is that
they replay the *measured* reply shape rather than the header's wording: a
request answers with one or more counting updates and then a final one, the
identifier moves per request and not per search, a matchless search answers once.
The two real runs count the same page both folded (8) and case-sensitively (7),
which is how the assertion proves `match_case` reached Blink instead of being
filtered on the host side, and they read `scrollTop` twice — before and after a
step — because the measurement that says a search does not move the page while a
step scrolls the match in is the whole reason no match rectangle is on the wire.

## Using it as a library

```c
GtkWidget        *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession  *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
if (!chromium_session_start (session, "https://example.com", &error))
  g_printerr ("%s\n", error->message);
```

`ChromiumSession` signals: `::changed` (uri / title / loading / history moved,
and the mute state landing), `::ready` (the helper answered its handshake),
`::load-end` (uri, HTTP code — it fires for failed loads too, with code 0),
`::load-error` (net error code, uri, name), `::renderer-exited` (termination
status, error code, message), `::script-result` (id, ok, JSON value, message),
`::download-begin` (id, url, suggested name, mime type, total bytes),
`::download-progress` (id, received, total, percent, speed), `::download-end`
(id, `complete`/`cancelled`/`interrupted`, path, received), `::find-result`
(search id, count, active ordinal, final), `::closed` (the
helper is gone and will not come back).
`chromium_session_set_muted ()` silences the page — its sound leaves the helper
through an unsandboxed audio process and lands on the host's speakers, and the
stream stays open while muted, so the desktop mixer cannot see the switch
(`docs/audio-path.md`).
A download is a question, not a command: nothing reaches the disk until the host
answers `::download-begin` with `chromium_session_accept_download (session, id,
path)` or refuses it with `chromium_session_reject_download (session, id)`, and
an unanswered one leaves no file at all (`docs/download-path.md`).
A search is the other shape of the same problem:
`chromium_session_find (session, pattern, forward, match_case, next)` answers
nothing and `::find-result` is the only reply, so the host renders a total when
and only when `final` arrives (`chromium_session_stop_finding ()` to end it, and
it replies even more silently — nothing). The demo wires both to a find bar that
opens with Ctrl+F and closes with Esc; the key controller runs in the capture
phase, because a `GtkEntry` would otherwise take Esc before the view forwards it
(`docs/find-path.md`).
`docs/control-channel.md` is the full command and event
list, and the record of the commands decided against.

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
| `sandbox-policy.md` | the sandbox decision: no setuid helper needed, what a host without user namespaces does to the run |
| `control-channel.md` | every command and event on the socket, what a failed load and a dead renderer look like, and the commands decided against |
| `audio-path.md` | where the page's sound actually goes: an unsandboxed audio service, straight to the host's speakers, and what muting it measures |
| `protocol-ceilings.md` | what real clients bind against the nested compositor, and the measured verdict on every protocol and version ceiling left un-advertised |
| `download-path.md` | the parked `OnBeforeDownload` gate, who decides the path, and the three CEF download behaviours only a measurement can tell you |
| `find-path.md` | a command that answers nothing, the reply shape only a measurement gives (one identifier per request, one to three updates per request), and what actually scrolls the page |

## Known limitations

- **No drag and drop out of the page.** `start_drag` is refused on purpose: a
  surface inside a widget has nowhere to be dropped. The version 3 requests are
  answered, they just do nothing.
- **One seat, one pointer, no touch.** The embedding widget is the only device
  the compositor knows about.
- **No pinch zoom, no tablet input, no middle-click paste.** Chromium binds
  `zwp_pointer_gestures`, `zwp_tablet_manager_v2` and the primary-selection
  manager against real compositors; the nested compositor does not advertise
  them, so the page loses exactly those features and nothing else
  (`docs/protocol-ceilings.md` has the measured verdict for every protocol left
  out).
- **A failed navigation reports twice, and `::load-end` still fires.** First the
  real code (`ERR_NAME_NOT_RESOLVED`), then `ERR_ABORTED` (-3) about a second
  later; the load-end arrives with HTTP code 0. Nothing is filtered, because
  picking which error a host should see is not this layer's call
  (`docs/control-channel.md`).
- **Only the renderer's death is reported.** `::renderer-exited` comes from CEF's
  render-process callback; when one of the other sandboxed children goes, the
  host hears nothing until the page stops answering.
- **A download can be accepted, refused or cancelled — not paused.**
  `CefDownloadItemCallback` also has `Pause` and `Resume`; nothing on the channel
  carries them yet. Of the three end states, `interrupted` is the one
  `test/download-gate.sh` cannot reach: the local server keeps answering until
  Chromium gives up on its own (`docs/download-path.md`).
- **Find walks matches one at a time.** CEF's `Find` has no "jump to match N",
  and a brand-new search selects nothing at all (the active ordinal is 0), so a
  host that wants the fifth match sends five steps. Measured once in three runs,
  the first step reported the third match instead — the ordinal is Blink's own
  ordering and the gate deliberately does not pin it (`docs/find-path.md`).
- **The page's sound bypasses the widget entirely.** Audio lives in an
  unsandboxed `audio.mojom.AudioService` utility and reaches the host's speakers
  as an ordinary desktop app; the nested compositor never sees it, and GNOME's
  mixer cannot tell a muted stream from a quiet one. `set_muted` is the only
  switch, and nothing announces that a page *started* making noise
  (`docs/audio-path.md`).
- **The renderer is sandboxed; the helper itself is not, and cannot be.** The
  browser process holds the Wayland connection, the GPU buffers and the control
  socket, so it stays unsandboxed by design — the trust boundary is the page
  code in the renderer, not `cefnest-helper`. On a host with neither
  unprivileged user namespaces nor a setuid `chrome-sandbox` the helper dies at
  startup instead of quietly running unsandboxed; `CHROMIUM_HELPER_NO_SANDBOX=1`
  is the way to say it on purpose (`docs/sandbox-policy.md`).
- **Native Wayland only, by design.** Under an X11 session GDK cannot give the
  widget a Wayland surface to hand to clients, so the component will not work
  there.
- The API is not stable: nothing here is installed, and `src/meson.build` builds
  a static library.

## License

LGPL-2.1-only — see `LICENSE`. The Chromium helper links CEF, which is BSD-3-Clause with
its own third-party notices; those are not part of this repository and are not
redistributed by it.
