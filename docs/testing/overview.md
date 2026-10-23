# Testing overview

Everything the tree runs to hold its rules: the meson suites, the unattended
battery, and the manual gates — what each one proves, the measured numbers
behind the assertions, and the mutation each assertion is proven against.
Build and run steps are in the README; this is the full write-up that used to
live there.

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
`multi-browser-gate.sh`, `sandbox-soak.sh`, `helper-signal-gate.sh`,
`event-shape-gate.sh`, `framing-gate.sh`, the opt-in real-page batch
`real-sites.sh`, and `real-site-run.sh`, the one-site runner that batch calls
per site.
`test/ci-session.sh` is how any of them runs with nobody's desktop touched.

`seat-selftest` starts the nested compositor, launches `shm-client` against it,
runs the scripted pointer/keyboard/text-input sequence and prints what the
client saw. Each mode asks for a different set of protocol objects:

```bash
./builddir/test/seat-selftest /tmp/x.png "$PWD/builddir/test/shm-client" --popup
```

with `--dmabuf`, `--text-input`, `--cursor-shape`, `--popup`, `--refused`,
`--pool-budget`, `--wrong-object`, `--pool-shrink`, `--empty-copy`,
`--popup-depth`, `--popup-cycle`, `--popup-overflow`, `--damage-scale`, `--drag-fake-serial`, `--drag-no-source`,
`--dnd-requests`, `--drag-out`, `--drag-in`, `--fullscreen`,
`--second-window`, `--gtk-client`. On a machine with no display each one exits 77,
which is what meson reads as SKIPPED — a skip on the same page as a pass is how a
display-dependent run hides an untested rule.

`map-cycle` is the other display-dependent run: it hides and shows the widget and
counts the signal slots GLib is left holding, so a handler armed on every `map`
without a take-down cannot pass it. The static half of the same rule,
`map-arms-audit`, reads the sources and catches what a program cannot count —
a `GtkEventController` has no public list. `docs/internals/map-cycle.md` is the write-up of
both, with the mutation each one is proven against.

`--pool-budget` is the one where the client is the attacker: it hands over sixteen
100 MB pools — each of them legal on its own — and keeps them all alive, and the
run fails unless the compositor takes some and refuses the next. It measured 10
accepted, 1 refused against the 1 GiB per-connection ceiling
(`docs/internals/resource-limits.md` §2).

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
(`docs/internals/clipboard-offer-during-client-init.md`).

The raw client does check one thing of its own, and the run stops on it: a
`wl_pointer.axis_stop` may not share a `wl_pointer.frame` with the `axis` event
it stops, which is the shape that makes Chromium throw the scroll delta away
(`docs/features/wheel-resolution-and-key-repeat.md`). `seat-selftest` waits for its
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
wire is not a gate (`docs/internals/empty-request-slot.md`). The step table and what the
run cannot yet say are in `docs/features/multi-browser.md`.

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
refusal, it aborts the compositor (`docs/internals/empty-request-slot.md`).

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
`docs/features/download-path.md` §6 has the matrix.

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
same run (`docs/features/clipboard-bridge.md` §5).

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
in `docs/testing/real-sites.md`.

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
either one with nobody's desktop touched. `docs/guides/control-channel.md` "通道读这一侧" is
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
`docs/internals/sandbox-policy.md` §7 and §8 are the numbers this keeps from going stale.

`test/helper-signal-gate.sh` is the exit-path half: it signals the helper the way a
session manager does — SIGTERM, then SIGHUP in a second run — and grades six
assertions per signal. Before sending anything it reads `SigCgt` out of
`/proc/PID/status`, because `CefInitialize()` puts those three signals back to
`SIG_DFL` on the way through (strace: about 2 ms after an install made before it), and
without that check "no handler was ever installed" and "one transcript line is missing"
grade the same. Then: the handler names the signal on the UI thread, `main()` reaches
its tail after `CefShutdown()`, the host never reports a signal death, and the profile's
`SingletonLock` is gone — that lock is the user-visible symptom, since the next helper on
the same profile reads it as "opening in an existing browser session". It runs hosted:
under `gnome-shell --headless` a standalone helper never returns from `CefInitialize()`
(measured on the current and the previous binary), which is what
`docs/internals/sandbox-policy.md` §9 records. Both runs open a real window, so it is a
documented manual gate, and `test/ci-all.sh` hosts it as the `signal` row.
`docs/features/session-lifecycle.md` §7 is the write-up, including the two defects this
gate surfaced while being written: a `HelperWindowDelegate` that kept a second owner of
its browser view — which made the *first* tab impossible to close, so the signal path
could never reach `QuitOnce()` — and a host dispose that force-exited a helper over the
helper's own `{"event":"closed"}`, cutting `CefShutdown()` off mid-call.

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
`docs/internals/frame-clock-pacing.md` §8 and §9 keep the whole round trip, including the freeze
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
to report and the script engine says it itself (`docs/features/session-lifecycle.md` §4–§6),
and that the
existing gates run inside it: `find-gate.sh` (34 assertions), `permission-gate.sh`
(58), `download-gate.sh` (54), `dnd-gate.sh` (59), `pixel-gate.sh` (12),
`primary-gate.sh` (2), `multi-browser-gate.sh` (6),
`sandbox-soak.sh` (12) and `meson test` (15/15) all pass unchanged, and
`menu-gate.sh` passed 45 of its 47 on that run —
the two it skips are the ones that assert a popup surface reached a screen, which a
display with no output device cannot do (`docs/testing/headless-ci.md` §5). That last row is
not reproducible on this laptop as of 2026-10-22: the headless shell stopped answering
the popup's `xdg_surface.configure`, so `gtk_popover_popup()` blocks the demo's main
thread inside a Wayland roundtrip and the run dies on its timeout — measured the same
at `HEAD` with the current changes taken back off, and clean on a real display, so it
is the outer compositor, not this layer (`docs/testing/headless-ci.md` §6.1). What it cannot
prove is frame pacing: a display with no output device never runs a second GTK
display cycle, which is exactly the failure mode `docs/internals/frame-clock-pacing.md` §8
documents, where a client left waiting on that cycle stopped animating and stopped
painting — and §9, where the fix for it silently froze the displays whose clock did
answer. `docs/testing/headless-ci.md` is the write-up.

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

## The demo's environment

The demo reads its whole configuration from the environment, because the point
of it is reproducible runs:

| Variable | What it does |
| --- | --- |
| `CHROMIUM_HELPER` | set (to anything) to force session mode; a path here must contain `/`, and it is used as written even when it does not exist |
| `CEFNEST_HELPER` | the same rule one step earlier: a path with `/` in it wins over `CHROMIUM_HELPER`. With neither set, the library resolves the helper itself — compiled-in install path, then the cmake tree it was built next to (`docs/guides/packaging.md`) |
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
| `CHROMIUM_HELPER_NO_SANDBOX` | set to exactly `1` to run the helper without Chromium's sandbox; anything else — including a typo — leaves it on (`docs/internals/sandbox-policy.md`) |
| `CEFNEST_DOWNLOAD_DIR` | where the demo panel auto-accepts a download; defaults to the XDG download directory (`docs/features/download-path.md`) |
| `CEFNEST_DOWNLOAD_ASK` | any value: every download waits for the 保存/丢弃 buttons or a script step instead of landing on disk |
| `CEFNEST_DOWNLOAD_NO_SAVE_AS` | any value: image downloads skip the `GtkFileDialog` and use the normal download-directory/ask path |
| `CEFNEST_PERMISSION` | `ask` (default: a row with 允许/拒绝 per request), `deny` / `allow` (answer on arrival, the row stays as the record), or `ignore` (do not take the question at all — the session then denies it, which is what a host with no permission UI does) (`docs/features/permission-prompt.md`) |
| `CEFNEST_DRAG_OUT` | what the demo does with a page drag that leaves the window. Default: take the payload and start the window's own `GdkDrag` with it; image bytes become a temp file offered as `GdkFileList`, text stays text. `take`: read and log what came out but do not start a drag of our own — what the automated runs use, because a scripted press leaves GDK no serial to begin one with. `off`: do not connect to `::drag-out` at all, so the page keeps its drag until the button lets go, which is what a host with no handler gets (`docs/features/drag-loop.md`) |
| `G_MESSAGES_DEBUG` | **space-separated** domains: `all`, or `nested chromium-view chromium-session` — a comma list matches nothing |
