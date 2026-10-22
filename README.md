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
| `NestedCompositor` | `src/nested.c` + `src/nested-{seat,dmabuf,xdg,cursor,text-input,data-device}.c` | the compositor: globals, surfaces, damage, input, clipboard, drag in both directions across the window edge |
| `ChromiumView` | `src/chromium-view.c` | the widget: hands its GdkSurface's input to the compositor, paints the composition |
| `ChromiumSession` | `src/chromium-session.c` | the control plane: starts and stops the helper, tracks page state, runs JavaScript, brokers downloads, find, the page's context menu, and its permission requests |

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
| meson / ninja / cmake | 1.7 / 1.12 / 3.16+ | meson 1.3 (the demo's `$ORIGIN` rpath is built with `fs.relative_to`) |
| CEF (`cef_binary_*_linux64_minimal` / `linuxarm64_minimal`) | 154.0.34+g14c5a08+chromium-154.0.8037.98 | any that builds against its own CMake helpers; arm64 URL path is wired but not yet tested on an arm64 host |

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

## Install

The component installs as a shared library with a pkg-config file, headers, and
the demo; `docs/packaging.md` is the write-up of where each piece goes and of how
a session decides which helper binary to start.

```bash
meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install
```

| Option | Default | What it decides |
| --- | --- | --- |
| `helper-path` | empty, derived from `prefix` + `libexecdir` + the host architecture | the path compiled into the library, used when nothing names one |
| `helper-build-dir` | `helper/build/Release` | what cmake left behind: the development fallback, and what gets installed |
| `install-helper` | `false` | also install the helper and the whole CEF runtime next to it — 1.5 GB, so it is opt-in |
| `install-demo` | `true` | install `cefnest-demo`, and the `shm-client` it falls back to when there is no Chromium |

`install-helper` on a tree where cmake has not run is a configure-time error, not
an empty directory with a confident "Installing subdir" line above it. An
installed helper needs nothing from the tree it was built in — the CEF runtime
travels next to it and is resolved from there, which is measured with the whole
`third_party/cef/dist` directory moved aside (`docs/packaging.md` §9).

## Run

```bash
# the real thing — no environment needed once cmake has built the helper,
# because the library finds it (docs/packaging.md §2)
CHROMIUM_URL=https://example.com ./builddir/src/cefnest-demo

# no Chromium involved: name a helper that does not exist, and the demo falls
# back to driving test/shm-client through the nested compositor
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo

# an installed one, from a prefix, with nothing in the environment
CHROMIUM_URL=https://example.com $HOME/.local/cefnest-stage/bin/cefnest-demo
```

`CHROMIUM_HELPER` still forces session mode with any value at all, which is how
`test/fake-helper.py` is driven through a script without a browser anywhere.

The demo reads its whole configuration from the environment, because the point
of it is reproducible runs:

| Variable | What it does |
| --- | --- |
| `CHROMIUM_HELPER` | set (to anything) to force session mode; a path here must contain `/`, and it is used as written even when it does not exist |
| `CEFNEST_HELPER` | the same rule one step earlier: a path with `/` in it wins over `CHROMIUM_HELPER`. With neither set, the library resolves the helper itself — compiled-in install path, then the cmake tree it was built next to (`docs/packaging.md`) |
| `CHROMIUM_URL` | page to open |
| `CHROMIUM_SESSION_SCRIPT` | file of one command per line: `load`, `back`, `forward`, `reload`, `reload-hard`, `stop`, `zoom-in`, `zoom-out`, `zoom-reset`, `js`, `js-expr`, `a11y-on`, `a11y-off`, `frames`, `wait`, `shot`, `dump`, `hover`, `click`, `drag`, `drag-hold`, `release`, `drop-in <x0> <y0> <x1> <y1> [text] [mimes]`, `drop-in-hold`, `drop-in-move <x> <y>`, `drop-in-drop`, `drop-in-cancel`, `surfaces`, `wheel`, `download-accept <path>`, `download-reject`, `download-accept-name <substring> <path>`, `download-reject-name <substring>`,
`new-tab <url>`, `new-window <url>`, `select-browser <id>`, `close-browser <id>`, `find <pattern>`, `find-case <pattern>`, `find-next`, `find-prev`, `find-stop`, `menu-pick <id>`, `menu-dismiss`, `perm-accept <id>`, `perm-deny <id>`, `perm-dismiss <id>`, `close`, `quit` — see `test/*.session`. Navigation, JS, zoom, mute, download, find, menu and permission steps need a helper session — any of them in a run that has none ends it with status 1 instead of being skipped, while `wait`, `shot`, `dump`, `surfaces`, `hover`, `click`, `drag`, `wheel`, `drop-in*` and `close` drive the nested compositor alone |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | grab the widget after N ms and write a PNG, then exit |
| `CHROMIUM_WINDOW` | `fullscreen` or `maximize`, for deterministic geometry |
| `CHROMIUM_CLIPSET` / `CHROMIUM_CLIPWATCH` | seed the host clipboard, and bridge it into the page |
| `CHROMIUM_PRIMARYSET` / `CHROMIUM_PRIMARYWATCH` | seed/read the host primary selection, the middle-click clipboard |
| `CHROMIUM_SPELLCHECK` | unset/0 disables spellcheck (the default): the helper passes `--disable-spell-checking` and turns `spellcheck=false` on every editable node. `1` restores Chromium spelling suggestions and the caret collapse around its menu |
| `CHROMIUM_A11Y` | `1` forces accessibility at helper startup. Without it the session watches `org.a11y.Status.ScreenReaderEnabled` on the session bus and sends the runtime `a11y` command when a screen reader appears/disappears |
| `CHROMIUM_FRAME_METRICS` | unset/0 stays quiet; `1` prints `demo: frame-count N` once per second. The same counter is `chromium_session_get_frame_count()` / the `frames` script step |
| `CHROMIUM_HELPER_NO_SANDBOX` | set to exactly `1` to run the helper without Chromium's sandbox; anything else — including a typo — leaves it on (`docs/sandbox-policy.md`) |
| `CEFNEST_DOWNLOAD_DIR` | where the demo panel auto-accepts a download; defaults to the XDG download directory (`docs/download-path.md`) |
| `CEFNEST_DOWNLOAD_ASK` | any value: every download waits for the 保存/丢弃 buttons or a script step instead of landing on disk |
| `CEFNEST_DOWNLOAD_NO_SAVE_AS` | any value: image downloads skip the `GtkFileDialog` and use the normal download-directory/ask path |
| `CEFNEST_PERMISSION` | `ask` (default: a row with 允许/拒绝 per request), `deny` / `allow` (answer on arrival, the row stays as the record), or `ignore` (do not take the question at all — the session then denies it, which is what a host with no permission UI does) (`docs/permission-prompt.md`) |
| `CEFNEST_DRAG_OUT` | what the demo does with a page drag that leaves the window. Default: take the payload and start the window's own `GdkDrag` with it; image bytes become a temp file offered as `GdkFileList`, text stays text. `take`: read and log what came out but do not start a drag of our own — what the automated runs use, because a scripted press leaves GDK no serial to begin one with. `off`: do not connect to `::drag-out` at all, so the page keeps its drag until the button lets go, which is what a host with no handler gets (`docs/drag-loop.md`) |
| `G_MESSAGES_DEBUG` | **space-separated** domains: `all`, or `nested chromium-view chromium-session` — a comma list matches nothing |

## Minimal host

`examples/minimal.c` is the three-object version — window, `ChromiumView`,
`ChromiumSession` — with no demo chrome. Build it with the library and run:

```bash
ninja -C builddir
CEFNEST_URL=https://example.com ./builddir/examples/cefnest-minimal
```

The required order, the lifecycle, and the API map for navigation/JS/downloads/
menus/primary/a11y are in `docs/minimal-host.md`.

`examples/host.c` is the next step up on the same three objects: the four signals
a page stands on an answer for. It names the path for `::download-begin` (and so
owns the whole of where a page's file lands), answers `::permission-request` with
an allow/deny row, reads `::script-result` back into a log pane, and keys a tab
row off `::browser-created`. `::context-menu` is deliberately *not* connected
there, so the menu that works for free is visible next to the ones a host owns.
It takes a script file so it can be tested with no keyboard:

```bash
printf 'load http://gate.test/download?name=report.pdf&bytes=4096\nwait 800\nsave /tmp/dl\nwait 1600\nquit\n' > /tmp/steps
CEFNEST_HOST_SCRIPT=/tmp/steps CEFNEST_HELPER=$PWD/test/fake-helper.py \
  ./test/ci-session.sh ./builddir/examples/cefnest-host http://gate.test/start
```

Verbs: `load tab select close js save drop allow deny wait quit`. What those runs
measured — including a tab that cannot be closed at all — is written down in
`docs/minimal-host.md` §5.

Both examples read their own two environment variables (the demo's table above
does not cover them — they belong to the examples, not to the demo):

| Variable | What it does |
| --- | --- |
| `CEFNEST_URL` | the page `chromium_session_start()` opens; unset or empty, both examples fall back to `https://example.com` (`examples/minimal.c:34`, `examples/host.c:1039`) |
| `CEFNEST_HOST_SCRIPT` | (`examples/host.c` only) a file of one verb per line, driven from `::ready`, so the example can be tested with no keyboard |

## Test

```bash
meson test -C builddir                       # 25 tests
meson test -C builddir --suite selftest      # the twenty seat selftests + map-cycle
meson test -C builddir --suite protocol      # the three source audits
meson test -C builddir --suite unit          # the helper-path resolution rule
```

`test/ci-all.sh` is the unattended battery: every gate below that a nested
session can host, one nested compositor per gate, verdict table at the end —
`event-shape-gate.sh` is the one control-channel gate it does not host. The
remaining scripts in `test/` are documented manual gates, which is why none of
them is a `meson test`: they open real windows on whoever's desktop is in front
of them — `download-gate.sh`, `find-gate.sh`, `dnd-gate.sh`, `menu-gate.sh`,
`permission-gate.sh`, `primary-gate.sh`, `pixel-gate.sh`,
`multi-browser-gate.sh`, `sandbox-soak.sh`, `event-shape-gate.sh`,
`framing-gate.sh`, the opt-in real-page batch `real-sites.sh`, and
`real-site-run.sh`, the one-site runner that batch calls per site.
`test/ci-session.sh` is how any of them runs with nobody's desktop touched.

`seat-selftest` starts the nested compositor, launches `shm-client` against it,
runs the scripted pointer/keyboard/text-input sequence and prints what the
client saw. Each mode asks for a different set of protocol objects:

```bash
./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --popup
```

with `--dmabuf`, `--text-input`, `--cursor-shape`, `--popup`, `--refused`,
`--pool-budget`, `--wrong-object`, `--pool-shrink`, `--empty-copy`,
`--popup-depth`, `--damage-scale`, `--drag-fake-serial`, `--drag-no-source`,
`--dnd-requests`, `--drag-out`, `--drag-in`, `--fullscreen`,
`--second-window`, `--gtk-client`. On a machine with no display each one exits 77,
which is what meson reads as SKIPPED — a skip on the same page as a pass is how a
display-dependent run hides an untested rule.

`map-cycle` is the other display-dependent run: it hides and shows the widget and
counts the signal slots GLib is left holding, so a handler armed on every `map`
without a take-down cannot pass it. The static half of the same rule,
`map-arms-audit`, reads the sources and catches what a program cannot count —
a `GtkEventController` has no public list. `docs/map-cycle.md` is the write-up of
both, with the mutation each one is proven against.

`--pool-budget` is the one where the client is the attacker: it hands over sixteen
100 MB pools — each of them legal on its own — and keeps them all alive, and the
run fails unless the compositor takes some and refuses the next. It measured 10
accepted, 1 refused against the 1 GiB per-connection ceiling
(`docs/resource-limits.md` §2).

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

`--drag-in` is the mode that leans on that hardest: the destination is the raw
client, so it is the one place that can tell whether the bytes it was handed are
the bytes it was promised, and it compares them itself and exits 1 on a mismatch
rather than leaving the verdict to a log line.

`--second-window` is the multi-window input gate, and it is the one run where the
client's picture *is* the assertion. One `shm-client` maps a second toplevel
(`nested-shm-test-second`) beside its first and paints the window `wl_keyboard`
named red (`#c04040`), the other green (`#40c040`), redrawing on every enter and
leave — two toplevels from one client, because two CEF windows are also two
toplevels from one helper. `seat-selftest` then names the input surface at each
window, calls `nested_compositor_clear_input_surface()` once from a window that
does not own the input and once from the one that does, and at every checkpoint
checks three things that can disagree: the compositor's own
`keyboard_focus`/`pointer_focus`, the `nested_compositor_get_toplevels()`
inventory, and the centre pixel of each toplevel pulled out with
`gdk_texture_download()`. Dropping the owner guard and dropping the retarget
inside `clear_input_surface` each turned a green run red, which is why the
pixels are in the assertion at all — a gate that only checks state and never the
wire is not a gate (`docs/empty-request-slot.md`). The step table and what the
run cannot yet say are in `docs/multi-browser.md`.

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

`test/dnd-gate.sh` is the drag half, six real-Chromium runs on
`test/dnd-page.html` and 59 assertions. It answers the question the whole
in-process route hanged on: whether a browser inside a widget honours a
`wl_data_device.enter` at all, or settles an in-window drag through its own
internal drag manager. The page's own event log says it honours it — the
coordinates the script fed the seat are the coordinates the page reports on
`dragenter`, `dragover` and `drop`. The other runs are the ones that catch a
state machine left behind: a destination that will not take the payload, a
second drag after the first, a drag held open so the drag image can be seen
joining and leaving the paint stack and then a click that says the seat came
back, a drag that leaves the window entirely — which the compositor answers
for by reading the payload off the page's own source, calling that drag off, and
printing what it handed to the host — and a drag the other way, which the
compositor brings in as the source and the page then negotiates. That last run
is four inbound drags because the direction's whole product is the verdict the
host has to be given back: taken over the drop target, given back over an
element that handles nothing, given back when the host calls the drag off, and
taken again afterwards. The measurement that rewrote the verdict: a page reads a
host drag's payload twice the moment it is entered, whether or not it then keeps
the drop, so only `wl_data_offer.finish` decides. The outbound run is also the
measurement that settles whether Blink serves a `send` for a mime no destination
ever accepted, and it runs with `CEFNEST_DRAG_OUT=take` so no host drag can be
started under a real pointer. Neither direction needs a browser for its protocol
half: `meson test -C builddir --suite selftest` has `seat-selftest-drag-out`,
where `shm-client --drag-out` is both the origin and the destination of a drag of
the three measured types, and `seat-selftest-drag-in`, where
`shm-client --drag-in` is an active destination that accepts a type, reads the
payload the way a page was measured to and exits non-zero if the bytes are not
the ones it was handed. The gate pipes both streams through `tee` because GLib's
writer sends DEBUG messages to stdout rather than stderr once a process is off a
tty, and the compositor's own trace is half of what is asserted.

`test/menu-gate.sh` is the context-menu half, eight runs — six against
`test/fake-helper.py`, two against real Chromium on `test/menu-page.html` — and
47 assertions. The fake half replays the constraint that decided the whole
design: Chromium holds **one** unanswered menu request, so five right clicks with
no answer produce one event and a page that saw all five. That is why the session
always answers — a default menu when the host does not take the signal, a dismiss
when there is no view or no command row to draw, and a dismiss on a navigation,
because a navigation measured as *not* closing a request the host owns. The real
half proves the round trip rather than the drawing: `menu-pick 117` is CEF's own
全选, and the page reads back `selectionEnd - selectionStart == 18`, so the
command ran in Blink and not in the host. Asserting that a menu appeared uses the
session's `context menu mapped=1 size=` line, not a `shot` — on Wayland the
popover is a surface of its own, outside the widget the grab covers. That line is
the one assertion a display without a monitor cannot satisfy (its popups are
created, laid out and never mapped), so inside `test/ci-session.sh` the gate
reports those two as skips and still exits green on the other 45.

`test/image-dnd-page.html` is the manual image-drag fixture: it carries a
draggable 8x8 PNG, and the host converts Chromium's
`application/octet-stream;name="download.png"` into a temp file plus
`GdkFileList` so a file manager can copy it. It is not part of a scripted gate;
the source-side GTK drag needs a real press serial.

`test/spellcheck-page.html` is the clean spellcheck fixture (one textarea, no
link/image/selection test content). Spellcheck is off by default; set
`CHROMIUM_SPELLCHECK=1` to exercise the suggestion menu and its caret collapse.
`test/primary-page.html` is the middle-click primary-selection fixture.
`CHROMIUM_PRIMARYSET`/`CHROMIUM_PRIMARYWATCH` seed and read the host primary;
a scripted left click focuses the textarea and button `274` (`BTN_MIDDLE`) pastes.
The host→page direction was measured with 274 bytes; the page→host direction
needs Chromium to create a primary source, which this CEF build did not do in the
same run (`docs/clipboard-bridge.md` §5).

`test/primary-gate.sh` is the page-selection half of the primary clipboard:
the helper injects a selection listener, the session writes the selected word to
Gtk's primary clipboard, and the gate reads it back with
`CHROMIUM_PRIMARYWATCH=1`. It runs in the headless ci session and is now one of
the default `ci-all` gates.

`test/real-sites.sh` is the real-page inspection batch. It runs one
`test/ci-session.sh` per site from `test/real-sites.tsv` (Bing, MDN, YouTube,
Google Maps, Excalidraw), then keeps the session, transcript, screenshot,
`readback.json` and a `summary.md` under `builddir/real-sites/`. It uses the
`js-expr` script step so pages with a strict `script-src` (MDN) can still answer:
`js` wraps code in the page's `eval`, which their CSP rejects. It is not part of
the default `ci-all` battery because the sites are external; `CI_ALL_REAL=1
./test/ci-all.sh real` is the opt-in line. The first measured batch is written up
in `docs/real-sites.md`.

`test/permission-gate.sh` is the permission half, ten runs — seven against
`test/fake-helper.py`, three against real Chromium on `test/permission-page.html`
— and 58 assertions. It never opens a camera or a microphone: every media ask in
the file ends in a deny, and the one accepted grant it asserts is a notification,
which has no device behind it. What the fake replays is the queue shape measured
against the browser — a media ask the host has not answered holds the media queue,
and the ask made behind it arrives as its own request only once the answer is back;
a prompt the host has not answered is dismissed as `ignore` the moment the next
prompt shows up. The real runs assert on what the page sees rather than on the
trace: `real-deny` compares ten readbacks word for word, `real-answer` proves the
host's answer reached the page (a denied geolocation is `code1`, an accepted
notification is `granted`), and `real-park` pins the media queue with two ordering
assertions plus four page readbacks. Each real run gets its own `XDG_CACHE_HOME`,
which is also how the cold-profile start was found: the helper needs about three
seconds to report `ready` on a profile nobody has used before, and a session
script that starts on a fixed timer loses its first `load` on the floor. Session
mode now starts the script from `::ready`.

Two gates watch the control channel from the peer's side. `test/event-shape-gate.sh`
sends every event name the dispatch compares against — read out of
`src/chromium-session.c`, not copied into the script, and a run that finds fewer than
20 names fails rather than grades nothing — with every field absent, which is the
shape of a helper from another version. It asserts no critical is printed and nothing
reaches the demo as `(null)`: with the field guards taken back out, the same stream
costs the host 18 criticals and then a SIGSEGV. `test/framing-gate.sh` gained a
`flood` half for the other rule — a line has to end. One run grades three things: a
legal 1 MB event must be read whole, a line holding a NUL must be dropped rather than
half-believed, and bytes with no newline must hit the 4 MiB ceiling and stop the
channel. Both open a real window, so both are documented manual gates on the same
convention as `menu-gate.sh`; `./test/ci-session.sh ./test/event-shape-gate.sh` runs
either one with nobody's desktop touched. `docs/control-channel.md` "通道读这一侧" is
the write-up, with the mutation each assertion is proven against.

`test/sandbox-soak.sh` is the sandbox half, and it asserts on the process tree
rather than on pixels: one session, 20 navigations over four local pages with a
hard reload every seventh, and a deliberate `kill -9` of every renderer at two
points inside the run. Eight groups of checks — the tree keeps its size (9
processes, 8 with `--type=`, 2 renderers, unchanged from the settled sample on),
nothing becomes a zombie, no child ever carries `--crashpad-handler-pid`, a killed
renderer's parent is not the browser process, the fd count stays inside
`FD_SLACK` (default 40) both at the end and at its peak, the page keeps answering
`js` through both kills, exactly two `renderer exited status=1 error=9` lines
appear, and nothing is left running when the demo exits. RSS is reported and not
pinned — it is a sawtooth. Measured run: 40 s, 19 samples, fd 379 → 378 (peak
386), 898 MB, 18 of 20 readbacks answered, 0 leftovers.
`docs/sandbox-policy.md` §7 and §8 are the numbers this keeps from going stale.

`test/pixel-gate.sh` is the only gate that looks at pixels instead of at logs. Every
other one reads a scroll position, a find count, a menu model — those say the page is
alive, none of them say a pixel of it arrived. `test/pixel-page.html` fills the
viewport with four flat fills, the demo dumps the view once, and the gate compares the
middle of each quadrant against the rgb the CSS asked for, byte for byte, on both the
composited widget and the client texture it was composited from. The buffer comes back
exactly `18,52,86 / 200,50,60 / 43,138,62 / 232,232,232`, and it does so on both
displays: inside `./test/ci-session.sh` and on this laptop's real desktop, 12
assertions and no skips either way.
That is also what makes the gate worth having. When it was first written it read
`#101828` — the buffer Chromium cleared and never painted — on the real display, with
the page's own `requestAnimationFrame` counter stopped at 4 callbacks across 4.5 s, and
the difference was filed as a property of a window nobody is looking at, with a skip to
match. It was a regression in the component: `bcbebc8` had taken the frame callbacks off
the clock path, so any display whose clock answered stopped answering its clients.
Putting the call back took the same run from 4 to 252 animation frames and the dump from
`#101828` to the page's own green, and the skip came out of the gate because a flat
buffer is a statement about the component, not about who is watching.
`docs/frame-clock-pacing.md` §8 and §9 keep the whole round trip, including the freeze
this gate would have caught and the one it nearly caught by accident of being skipped.

`test/ci-session.sh` is the one gate runner that does not open a window on the
desktop of whoever is sitting here. It starts a nested compositor — GNOME Shell 48
in `--headless` mode, which is mutter with no output device — inside its own HOME,
its own XDG directories and its own D-Bus bus, and runs whatever it is handed
against that display:

```bash
./test/ci-session.sh                        # proof: a window maps, a page loads
./test/ci-session.sh ./test/find-gate.sh    # any gate, unchanged
```

The isolation is not tidiness: a nested shell started against the real HOME joined
the host's session bus and activated nautilus, the Tracker miner and the app-grid
autofit, so an unattended run moved the desktop of the person using the machine.
The compositor is a session leader of its own, so one signal to its process group
takes the bus, the portal and gvfsd with it, and the run asserts that nothing is
left behind. What it proves is that a real window maps, that a real Chromium
session with the sandbox on loads a page and answers JavaScript, that a scripted
run which cannot show its page fails in seconds and names the cause — the helper
dying before it is ready, dying without ever connecting, closing the browser of
its own accord, or no helper to start at all, in which case there is no `::closed`
to report and the script engine says it itself (`docs/session-lifecycle.md` §4–§6),
and that the
existing gates run inside it: `find-gate.sh` (34 assertions), `permission-gate.sh`
(58), `download-gate.sh` (54), `dnd-gate.sh` (59), `pixel-gate.sh` (12),
`primary-gate.sh` (2), `multi-browser-gate.sh` (6),
`sandbox-soak.sh` (12) and `meson test` (15/15) all pass unchanged, and
`menu-gate.sh` passed 45 of its 47 on that run —
the two it skips are the ones that assert a popup surface reached a screen, which a
display with no output device cannot do (`docs/headless-ci.md` §5). That last row is
not reproducible on this laptop as of 2026-10-22: the headless shell stopped answering
the popup's `xdg_surface.configure`, so `gtk_popover_popup()` blocks the demo's main
thread inside a Wayland roundtrip and the run dies on its timeout — measured the same
at `HEAD` with the current changes taken back off, and clean on a real display, so it
is the outer compositor, not this layer (`docs/headless-ci.md` §6.1). What it cannot
prove is frame pacing: a display with no output device never runs a second GTK
display cycle, which is exactly the failure mode `docs/frame-clock-pacing.md` §8
documents, where a client left waiting on that cycle stopped animating and stopped
painting — and §9, where the fix for it silently froze the displays whose clock did
answer. `docs/headless-ci.md` is the write-up.

`test/ci-all.sh` is the one command that runs the whole battery through it — proof,
`meson test` and all nine gates, each in its own nested compositor, one verdict table
with the skipped checks counted in a column of their own, non-zero exit if anything
failed. Measured on this laptop on 2026-10-21: 370 s for the nine runs — 273 gate
assertions plus `meson test` 14/14, with 2 checks skipped.

```bash
./test/ci-all.sh                    # everything
./test/ci-all.sh find menu          # only these two
CI_ALL_REAL=1 ./test/ci-all.sh real # external sites, opt-in
```

## Using it as a library

`meson install` puts `libcefnest.so` in `libdir`, three headers in
`includedir/cefnest` (`nested.h`, `chromium-view.h`, `chromium-session.h`) and
`cefnest.pc` under it, so a host builds with `pkg-config`:

```bash
cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

A binary that lives outside the prefix still needs `LD_LIBRARY_PATH` or an
`ldconfig` entry — the installed demo does not, because its `DT_RUNPATH` is
`$ORIGIN`-relative (`docs/packaging.md` §4).

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
(search id, count, active ordinal, final), `::context-menu` (the click's x, y and
the model as a `GList` of rows — returning TRUE means the host draws it),
`::permission-request` (the helper's id, `media` or `prompt`, the requesting
origin, a `GList` of permission names and the raw mask — returning TRUE means the
host answers it), `::permission-dismissed` (id, result: the request Chromium took
back from under the host), `::zoom-changed` (the CEF zoom level the helper read
back after a zoom command or a main-frame load; 0.0 is 100 %, 0.5 is one Chrome UI
step), `::primary-selection` (the selected page text pushed to the host, which
writes it to Gtk's primary clipboard), `::copy-text` (a URL chosen from a link/image/media menu row — the host
owns the clipboard), `::open-url` (a URL chosen from a new-window menu row — the
demo asks the shared helper for a standalone browser, which gets its own GTK
window; a library host can route it however it likes), `::page-message`
(the page's `cefnestPostMessage()` payload as one opaque JSON string — the host
owns whatever schema is inside it; neither side of this relay caps the size, so a
very large payload is an ordinary event line under the host's 4 MiB ceiling —
docs/control-channel.md), `::popup-requested`
(the target URL of a `target=_blank`, `window.open` or middle-click — **the URL
is all the signal carries**: the disposition and the gesture stay on the socket
event, so a host cannot tell a background tab from a foreground one from the
callback, and the helper has already decided what to do with it),
`::browser-created` (id, url, and `windowed`: TRUE means this browser got its own
host window instead of another tab in the shared view), `::browser-selected`
(the id that now owns the shared view), `::browser-closed` (the id that is gone —
it arrives for a windowed browser and **never for a tab**, because the helper's
close is a silent no-op there, see Known limitations), `::closed`
(the helper is gone and will not come back, with which of the four reasons: the
host stopped it, the helper closed the browser, its channel hung up, or the
process was reaped — a death reports either of the last two, which is why an
unattended run says *that* rather than guessing).
`chromium_session_set_muted ()` silences the page — its sound leaves the helper
through an unsandboxed audio process and lands on the host's speakers, and the
stream stays open while muted, so the desktop mixer cannot see the switch
(`docs/audio-path.md`).
`chromium_session_zoom (session, "in"/"out"/"reset")` changes the page's CEF zoom
level in half-level steps; Chromium persists the level per origin, and the
helper answers with `::zoom-changed` rather than leaving the host to guess
whether the change landed. `chromium_session_get_zoom_level ()` is the last
level it reported. The demo binds Ctrl+plus, Ctrl+minus and Ctrl+0.
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
A right click is the third shape, and one of two that must be answered: CEF hands
over its menu model and expects exactly one reply, and while that reply is
outstanding the browser will not ask again — five unanswered right clicks are one
event. `::context-menu` returns TRUE to take the menu over, and then exactly one of
`chromium_session_context_menu_select (session, id)` (runs CEF's own command for
that id — 全选 measured as selecting in the page) or
`chromium_session_context_menu_dismiss (session)` has to follow. Return FALSE, or
connect nothing, and the session draws the model itself as a popover over the
view, so a right click is never a dead gesture (`docs/context-menu.md`).
A permission request is the fourth shape and the same contract with a page standing
on the answer: an unanswered `getUserMedia` leaves the promise pending *and* holds
the media queue behind it, and an unanswered prompt is dismissed as `ignore` the
moment Chromium shows the next one. `chromium_session_permission_respond (session,
id, result)` sends the answer, and `accept` means "all of it" — CEF requires the
granted mask to equal the requested one, so a host cannot give the microphone
without the camera; half a grant is a deny. Return FALSE, or connect nothing, and
the session denies on arrival rather than leaving the page waiting
(`docs/permission-prompt.md`).
`docs/control-channel.md` is the full command and event
list, and the record of the commands decided against.

## Where the detail is

`docs/` holds the write-ups — Chinese, with the measured before/after evidence.
They are the record of *why* the code is shaped like this:

| Document | About |
| --- | --- |
| `nested-split.md` | how `nested.c` became per-protocol modules |
| `frame-clock-pacing.md` | painting on the GTK frame clock instead of a timer, the watchdog for the display cycles that never come, and the round that found the watchdog had quietly taken the clock path off the frames |
| `popup-compositing.md` | the surface stack, placement and dismissal of page popups |
| `clipboard-bridge.md` | `wl_data_device` ↔ `GdkClipboard` in both directions |
| `session-lifecycle.md` | starting, stopping and dying with the helper |
| `resource-limits.md` | shm pool caps per pool and per connection, allocation checks, the dmabuf fd leak |
| `empty-request-slot.md` | a NULL request slot killing the whole desktop |
| `wheel-resolution-and-key-repeat.md` | `axis_value120`, one frame per gesture, auto-repeat bookkeeping |
| `clipboard-offer-during-client-init.md` | a legal `data_offer` sent one dispatch too early kills a GTK client |
| `sandbox-policy.md` | the sandbox decision: no setuid helper needed, what a host without user namespaces does to the run, the soak that keeps the process tree and the fd count flat through repeated navigation and two `kill -9`'d renderers (now `test/sandbox-soak.sh`), and why there is no crash handler here to fix or to turn off |
| `control-channel.md` | every command and event on the socket, what a failed load and a dead renderer look like, and the commands decided against |
| `zoom-path.md` | CEF zoom levels, the half-level step, per-origin persistence, why `load-end` re-reports the level, and the demo's keyboard bindings |
| `real-sites.md` | the five-site real-page inspection batch: one nested session per site, screenshots, JS readbacks, 3-second rAF counts, and the measured H.264/VP9 result |
| `pdf-path.md` | the measured PDF-viewer result: thumbnails/page controls in the widget dump, and what was not tested (forms, annotations, printing) |
| `multi-browser.md` | the single-helper browser registry, tab strip, standalone host windows, protocol ids, the per-window input hand-back rule and its gate, and the remaining popup caveat |
| `cnb-ci.md` | the cnb.cool entry point, the self-hosted runner it expects, and the one-time native/CEF prerequisites |
| `structural-decisions.md` | the two items intentionally left for a product decision: browser routing for tabs/windows, and arm64/loong64 CEF supply |
| `frame-metrics.md` | the compositor frame counter / script/env export and the software-path pool mmap + damage shadow copy |
| `minimal-host.md` | the three-object host, the required order, a map of which API/signal to add for navigation, JS, downloads, menus, primary selection and a11y, and §5: the medium host example, what its scripted runs measured, and the tab that cannot be closed |
| `audio-path.md` | where the page's sound actually goes: an unsandboxed audio service, straight to the host's speakers, and what muting it measures |
| `protocol-ceilings.md` | what real clients bind against the nested compositor, and the measured verdict on every protocol and version ceiling left un-advertised |
| `download-path.md` | the parked `OnBeforeDownload` gate, who decides the path, and the four CEF download behaviours only a measurement can tell you |
| `find-path.md` | a command that answers nothing, the reply shape only a measurement gives (one identifier per request, one to three updates per request), and what actually scrolls the page |
| `drag-loop.md` | the in-page drag choreography, the measured proof that a `wl_data_device.enter` reaches the page, what the destination actually accepts, why the fd relay is not what carries a same-page payload, how a drag that crosses the window edge is read off the page's source and handed to the host (image bytes become a temp file offered as a file list), and how the same crossing in reverse makes the compositor the source — including the measurement that says a page reads a host drag the moment it is entered, so only `finish` can decide the verdict |
| `context-menu.md` | the one constraint that decided the whole design (Chromium holds a single unanswered menu request), what CEF's Alloy default model really contains, the helper-added link/image/media rows, and `Continue(id)` proven inside the page |
| `permission-prompt.md` | which asks never reach the host at all, the two queues that make an unanswered request cost the page its next question too, and why `accept` can only ever mean "the whole mask" |
| `packaging.md` | what installs where, the five-step rule by which a session finds its helper, why `get_option('libexecdir')` must be joined to the prefix before it is compiled in, and the two fallbacks that deliberately run in opposite directions |
| `headless-ci.md` | running the gates against a nested mutter nobody can see: why the session needs its own HOME and bus, how one signal reaps the whole tree, and what a display with no output device can and cannot prove |
| `a11y.md` | whether a screen reader can reach the page inside the nested architecture: it can, because Alloy's windowed browser in `STATE_ENABLED` is Chromium's own Complete AT-SPI mode; the session-bus watcher that switches it, and what ORCA saw |
| `dmabuf-formats.md` | the feedback format table decided by what this process can import — GDK's advertised set — instead of the static four-format advertisement, with the measured consequence for CEF's all-linear buffers |
| `fullscreen.md` | Alloy only *notifies* on `requestFullscreen()` — the client must trigger the transition; the wiring from `OnFullscreenModeChange` down to `xdg_toplevel.set_fullscreen`, and the measured before |
| `map-cycle.md` | everything armed on every `map` needs a take-down path (the M11 finding: N map cycles, N copies of a handler); the two gates that hold the rule and the mutation each is proven against |

## Known limitations

- **Drag out and drag in both work end to end, verified by hand.** A drag
  that starts and ends inside one page runs the whole `wl_data_device`
  negotiation with the page's own drag image composited on top. Going out,
  the compositor reads the payload off the page's own source, retires that
  drag and hands mime, bytes and the type list to the host, which starts a
  `GdkDrag` on the toplevel native surface (`docs/drag-loop.md` §6). Text is
  carried as text; Chromium's image drag arrives as
  `application/octet-stream;name="download.png"`, so the demo writes a temp
  file and offers a `GdkFileList`, which is what a file manager can copy.
  Coming in, a `GtkDropTarget` on the view accepts file lists and strings:
  the enter prefetches the payload bytes over the `GdkDrop` (capped at
  32 MiB), the compositor introduces them as a `wl_data_offer`, the page
  negotiates and reads them, and the verdict comes back on the synthetic
  drop (`docs/drag-loop.md` §7). Both directions passed a hand drag (text
  out to an editor, text in from it).
- **One seat, one pointer, no touch.** The embedding widget is the only device
  the compositor knows about.
- **Independent host windows get the input, and a gate says so.** A view that
  names itself with `nested_compositor_set_input_surface()` takes the pointer and
  the keyboard, and the hand-back is explicit rather than self-healing: a view
  whose GTK window closes or hides calls
  `nested_compositor_clear_input_surface()`, which drops the override only when
  that view is the one currently named — so a second window closing later cannot
  strip routing from the window that has since taken it over — and retargets the
  seat in the same call instead of on the next event. What the compositor believes
  is then checked against what the client paints, by
  `seat-selftest --second-window` (`docs/multi-browser.md`). One path is still
  outside that guarantee: a drag **started** in an independent window. The
  out-of-bounds handover creates its `GdkDrag` on the demo's main toplevel
  (`docs/drag-loop.md` §6), so such a drag leaves from the wrong surface.
- **A tab-mode browser cannot be closed, and the request is dropped in
  silence.** `chromium_session_close_browser()` on a windowed browser runs the
  whole measured sequence (`browser N closing` → `browser N closed` →
  `browser 1 selected`); on a tab the helper detaches the view and calls
  `CloseBrowser`, and nothing comes back — no `OnBeforeClose`, so no
  `::browser-closed` either. An id that does not exist answers
  `status: no-such-browser`, which is how a host tells "already gone" from
  "silently refused" (code-review-2026-10-22.md M22; the three-way probe is in
  `docs/multi-browser.md` §1 and `docs/minimal-host.md` §5). The same asymmetry
  shows up one signal earlier: `::popup-requested` carries only the URL, so a
  host cannot see whether the page asked for a background tab or a foreground
  window — that lives on the socket event's `disposition`, which the signal
  does not forward.
- **H.264/MP4 is absent from this CEF distribution.** Real YouTube reports
  `videoReady=4` but `canPlayType('video/mp4; codecs="avc1.42E01E"')` is empty;
  VP9 WebM answers `probably`, and YouTube still plays through VP9. MP4/H.264-only
  pages will fail until the distribution gets a codec decision. Measured in
  `docs/real-sites.md` §3.1.
- **No pinch zoom and no tablet input.** Chromium binds `zwp_pointer_gestures`
  and `zwp_tablet_manager_v2` against real compositors; the nested compositor
  does not advertise them, so the page loses exactly those features and nothing
  else (`docs/protocol-ceilings.md`). Primary selection (middle-click paste) is
  bridged separately and text-only (`docs/clipboard-bridge.md` §5).
- **A failed navigation reports twice, and `::load-end` still fires.** First the
  real code (`ERR_NAME_NOT_RESOLVED`), then `ERR_ABORTED` (-3) about a second
  later; the load-end arrives with HTTP code 0. Nothing is filtered, because
  picking which error a host should see is not this layer's call
  (`docs/control-channel.md`).
- **Only the renderer's death is reported, and only how it died.** `::renderer-exited`
  comes from CEF's render-process callback — `kill -9` measured as `status=1` with
  error code 9, which is the OS-level wait status and nothing more: the minimal CEF
  distribution ships no crash handler, and no process in this tree carries a
  `--crashpad-handler-pid`, so there is no minidump and no stack behind it
  (`docs/sandbox-policy.md` §8). When one of the other sandboxed children goes, the
  host hears nothing until the page stops answering.
- **A download can be accepted, refused, cancelled or paused/resumed.**
  `CefDownloadItemCallback::Pause/Resume` are wired as `download-pause` /
  `download-resume`; the terminal event is still CEF's `download-end`. Of the
  three end states, `interrupted` is the one `test/download-gate.sh` cannot
  reach: the local server keeps answering until Chromium gives up on its own
  (`docs/download-path.md`). And `complete` is not the moment the file appears:
  when the bytes arrived while the gate was still parked, the file is on disk as
  soon as the path is named but CEF's terminal event follows 3–6 s later, batched
  with Chromium's download-database flush (measured 3.24 / 4.12 / 5.72 s, so the
  gate waits 9 s — `docs/download-path.md` §4.4).
- **Find walks matches one at a time.** CEF's `Find` has no "jump to match N",
  and a brand-new search selects nothing at all (the active ordinal is 0), so a
  host that wants the fifth match sends five steps. Measured once in three runs,
  the first step reported the third match instead — the ordinal is Blink's own
  ordering and the gate deliberately does not pin it (`docs/find-path.md`).
- **The helper extends the context menu; the host still cannot extend it.** A right
  click comes to the host as a model and goes back as a command id. The helper adds
  link/image/media rows from `CefContextMenuParams` (open in a new window, copy the
  address, save an image) and executes them; a host that wants its own rows still
  needs those params over the channel, which is the next protocol step. Image save
  now asks for a path with `GtkFileDialog`; `CEFNEST_DOWNLOAD_NO_SAVE_AS` opts out
  and returns to the download directory. Check and radio marks are not drawn and a
  submenu row does not open — the menu is rebuilt from a fresh model after every
  command, so a mark would have nothing to remember.
- **A permission can be granted or denied, not narrowed.** A media request has to be
  answered with the exact mask it asked for — `Continue()` requires `allowed_permissions`
  to match what was requested — so "microphone yes, camera no" does not exist here. And a request the host sits on
  costs more than silence: the page's promise never settles, the media queue stays
  blocked behind it, and measured twice, the ask made immediately after a held
  prompt disappears without ever reaching the host. Three asks never reach it at
  all (`navigator.storage.persist()`, `permissions.query()`, idle detection — they
  do not go through `CefPermissionHandler`), so no host policy can cover them, and
  nothing is remembered between requests: asking the same permission twice reaches
  the handler twice (`docs/permission-prompt.md`).
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
- The API is not stable. It *is* installed now — `src/meson.build` builds
  `libcefnest.so.0.0.1` with a `cefnest.pc` next to it — but nothing waits for a
  soname bump: a signal can gain a parameter (as `::closed` did) and a header can
  change shape between releases, so a host links against a build it built itself.

## License

LGPL-2.1-only — see `LICENSE`. The Chromium helper links CEF, which is BSD-3-Clause with
its own third-party notices; those are not part of this repository and are not
redistributed by it.
