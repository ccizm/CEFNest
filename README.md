# CEFNest

English | [简体中文](README.zh-CN.md)

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
| CEF (`cef_binary_*_linux64_minimal` / `linuxarm64_minimal`) | verified stable 154.0.34+g14c5a08+chromium-154.0.8037.98 | any that builds against its own CMake helpers; arm64 URL path is wired but not yet tested on an arm64 host |

`wayland-scanner` must be on `PATH` (it generates the protocol code at build
time — nothing is checked in).

Running a page needs what Chromium's sandbox needs: working unprivileged user
namespaces. The development machine has them (`unprivileged_userns_clone=1`, no
AppArmor restriction); a host that restricts them is a packaged-helper problem
and is written up under **Install** and in `docs/internals/sandbox-policy.md` §6.

## Build

Two stages, and the first is optional.

```bash
# 1. fetch the verified stable CEF binary distribution (gitignored, ~500 MB
#    download, 1.5 GB unpacked); CEF_VERSION=latest takes the newest stable
./scripts/fetch-cef.sh

# 2. the Chromium side — picks up third_party/cef/dist automatically
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. the widget, the demo, the tests
meson setup builddir
ninja -C builddir
```

`scripts/fetch-cef.sh` fetches the verified stable 154.0.34 by default. Use
`CEF_VERSION=latest` for whatever the CEF index currently marks stable, or
`CEF_VERSION=<full CEF version>` to pin another build; `CEF_CHANNEL=any` takes
the newest build on any channel. A different distribution can be passed with
`-DCEF_ROOT=…`. A CEF version bump means re-running the full gate — a Chromium
milestone also changes what Wayland protocols the client asks for
(docs/internals/protocol-ceilings.md is the re-measurement record).

Skip stage 1 and everything still builds: the demo then drives
`test/shm-client` instead of Chromium, which is enough to exercise the
compositor, the seat, the clipboard bridge and the popups.

## Install

The component installs as a shared library with a pkg-config file, headers, and
the demo; `docs/guides/packaging.md` is the write-up of where each piece goes and of how
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
`third_party/cef/dist` directory moved aside (`docs/guides/packaging.md` §9).

The CNB release pipeline packs the install tree into two packages, as both debs
and rpms. `cefnest` is the component itself: the GTK 4 widget library,
`cefnest-helper` and the complete CEF runtime (about 1.5 GB installed). The
library installs system-wide as `libcefnest.so.0`, depends only on system
libraries, and installing it is enough to run a host.
`cefnest-dev` adds the headers, the unversioned `.so` link and `cefnest.pc`;
it depends on the exact `cefnest` version, and a host builds with
`pkg-config --cflags --libs cefnest`. The demo and `shm-client` are
development/test tools and are not packaged — they travel in
`cefnest-build.tar.gz`. Both packages share one version — CEF stable `X.Y.Z`
plus a revision (`154.0.34-8`) — and are built and released together. Upgrades
from the four-package -6 layout are handled by deb `Replaces`/`Breaks` and rpm
`Obsoletes`, so they need no manual removal. The pipeline builds on version
tags only; see `docs/testing/cnb-ci.md`.

`cefnest-helper` starts Chromium with the sandbox **on** and installs
`chrome-sandbox` as plain 0755 — no setuid bit, no AppArmor profile shipped with
it. On a host with working unprivileged user namespaces that is the whole story:
no install step, no root. On a host that restricts them (Ubuntu 24.04 enables
`apparmor_restrict_unprivileged_userns` by default; the knob exists from 23.10)
the packaged helper stops with `No usable sandbox!` and says so on stderr; it
never silently drops the sandbox.
The two ways out are a per-application AppArmor profile allowing userns for
`cefnest-helper`, or `CHROMIUM_HELPER_NO_SANDBOX=1` in the host's environment.
The `chmod 4755 chrome-sandbox` route Chromium's own message mentions is not
what this project ships or has measured. `docs/internals/sandbox-policy.md` §6
is the write-up; `scripts/ci-smoke-packages.sh` asserts the 0755 mode and starts
the installed helper on every release.

## Run

```bash
# the real thing — no environment needed once cmake has built the helper,
# because the library finds it (docs/guides/packaging.md §2)
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/src/cefnest-demo

# no Chromium involved: name a helper that does not exist, and the demo falls
# back to driving test/shm-client through the nested compositor
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo

# an installed one, from a prefix, with nothing in the environment
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest $HOME/.local/cefnest-stage/bin/cefnest-demo
```

`CHROMIUM_HELPER` still forces session mode with any value at all, which is how
`test/fake-helper.py` is driven through a script without a browser anywhere.

The demo reads its whole configuration from the environment, because the point
of it is reproducible runs. The core:

| Variable | What it does |
| --- | --- |
| `CHROMIUM_URL` | page to open |
| `CEFNEST_HELPER` | a path with `/` in it wins over `CHROMIUM_HELPER`; with neither set, the library resolves the helper itself (`docs/guides/packaging.md`) |
| `CHROMIUM_HELPER` | set (to anything) to force session mode; a path here must contain `/`, and is used as written even when it does not exist |
| `CHROMIUM_SESSION_SCRIPT` | a file of one command per line (`load`, `js`, `find`, `menu-pick`, `download-accept`, …); the full verb list and which steps need a helper session are in `docs/testing/overview.md` |
| `CHROMIUM_DUMP` / `CHROMIUM_DUMP_DELAY` | grab the widget after N ms and write a PNG, then exit |
| `CHROMIUM_HELPER_NO_SANDBOX` | set to exactly `1` to run the helper without Chromium's sandbox; anything else — including a typo — leaves it on (`docs/internals/sandbox-policy.md`) |
| `CHROMIUM_USER_AGENT` | a default User-Agent baked into the helper's Chromium at startup; hosts put it in their own environment before `chromium_session_start()` — `examples/cefnest-useragent` switches it live by restarting the session |
| `G_MESSAGES_DEBUG` | **space-separated** domains: `all`, or `nested chromium-view chromium-session` — a comma list matches nothing |

The rest — download/permission/drag panel modes, clipboard and primary seeds,
a11y, spellcheck, frame metrics, window geometry — is documented together with
the gates in `docs/testing/overview.md`.

## Minimal host

`examples/minimal.c` is the three-object version — window, `ChromiumView`,
`ChromiumSession` — with no demo chrome; `CEFNEST_URL` names the page it opens.
`examples/host.c` is the next step up on the same three objects: the four
signals a page stands on an answer for — `::download-begin` (so the host owns
where a page's file lands), `::permission-request`, `::script-result`,
`::browser-created` — driven testably without a keyboard by a script file
(`CEFNEST_HOST_SCRIPT`, verbs `load tab select close js save drop allow deny
wait quit`). The required order, the lifecycle, the API map for
navigation/JS/downloads/menus/primary/a11y, and what the scripted runs measured
are in `docs/guides/minimal-host.md`.

```bash
ninja -C builddir
CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-minimal
```

## Test

```bash
meson test -C builddir                       # 25 tests
meson test -C builddir --suite selftest      # the twenty seat selftests + map-cycle
meson test -C builddir --suite protocol      # the three source audits
meson test -C builddir --suite unit          # the helper-path resolution rule
```

The rest of the tree's checking is gates that open real windows, which is why
none of them is a `meson test`. `test/ci-session.sh` runs any of them against a
nested headless compositor — its own HOME, XDG directories and D-Bus bus, so
nobody's desktop is touched — and `test/ci-all.sh` is the one command that runs
the whole battery that way, one nested compositor per gate, verdict table at
the end (measured 370 s: 273 gate assertions plus `meson test` 14/14).

```bash
./test/ci-session.sh ./test/find-gate.sh     # any gate, headless
./test/ci-all.sh                             # everything
CI_ALL_REAL=1 ./test/ci-all.sh real          # external sites, opt-in
```

What each gate proves, the measured numbers behind the assertions, and the
mutation each assertion is proven against: `docs/testing/overview.md`. The
headless runner's own write-up is `docs/testing/headless-ci.md`, the cnb.cool
entry point is `docs/testing/cnb-ci.md`, and the real-page batch is
`docs/testing/real-sites.md`.

## Using it as a library

`meson install` puts `libcefnest.so` in `libdir`, three headers in
`includedir/cefnest` (`nested.h`, `chromium-view.h`, `chromium-session.h`) and
`cefnest.pc` under it, so a host builds with `pkg-config`:

```bash
cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

A binary that lives outside the prefix still needs `LD_LIBRARY_PATH` or an
`ldconfig` entry — the installed demo does not, because its `DT_RUNPATH` is
`$ORIGIN`-relative (`docs/guides/packaging.md` §4).

```c
GtkWidget        *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession  *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
if (!chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error))
  g_printerr ("%s\n", error->message);
```

The session's contract is that a page stands on the host's answer: a download,
a find, a context menu and a permission request are all questions the session
brokers, each with its own reply shape and its own cost for leaving it
unanswered. Every signal and call, with the measured shape it carries, is
documented inline in `src/chromium-session.h`; the command and event list of
the channel underneath — and the commands decided against — is
`docs/guides/control-channel.md`.

## Documentation

`docs/` holds the write-ups — Chinese, with the measured before/after
evidence. `docs/README.md` is the index: what lives where, and which one to
read first.

## Known limitations

One line each; the full record, with every measurement behind it, is
`docs/known-limitations.md`.

- Drag out and drag in both work end to end, hand-verified.
- One seat, one pointer, no touch.
- Independent host windows get the input, and a gate says so — but a drag
  started in one leaves from the wrong surface.
- `::popup-requested` carries only the URL, so a host cannot tell a background
  tab from a foreground window.
- H.264/MP4 is absent from this CEF distribution; VP9 plays.
- No pinch zoom and no tablet input.
- A failed navigation reports twice, and `::load-end` still fires.
- Only the renderer's death is reported, and only how it died.
- A download can be accepted, refused, cancelled or paused/resumed — but
  `complete` is not the moment the file appears.
- Find walks matches one at a time.
- The helper extends the context menu; the host still cannot.
- A permission can be granted or denied, not narrowed — and an unanswered one
  costs the page its next question too.
- The page's sound bypasses the widget entirely; `set_muted` is the only switch.
- The renderer is sandboxed; the helper itself is not, and cannot be.
- On a host that restricts unprivileged user namespaces, the packaged helper
  refuses to start until it gets an AppArmor profile or
  `CHROMIUM_HELPER_NO_SANDBOX=1`.
- Native Wayland only, by design.
- The API is not stable.

## License

LGPL-2.1-only — see `LICENSE`. The Chromium helper links CEF, which is BSD-3-Clause with
its own third-party notices; those are not part of this repository and are not
redistributed by it.
