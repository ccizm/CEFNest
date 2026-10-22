# Known limitations

Each item below carries its measurement — where the number came from, or the
transcript that showed the behaviour. The README keeps a one-line summary of
this list; this is the full record.

- **Drag out and drag in both work end to end, verified by hand.** A drag
  that starts and ends inside one page runs the whole `wl_data_device`
  negotiation with the page's own drag image composited on top. Going out,
  the compositor reads the payload off the page's own source, retires that
  drag and hands mime, bytes and the type list to the host, which starts a
  `GdkDrag` on the toplevel native surface (`docs/features/drag-loop.md` §6). Text is
  carried as text; Chromium's image drag arrives as
  `application/octet-stream;name="download.png"`, so the demo writes a temp
  file and offers a `GdkFileList`, which is what a file manager can copy.
  Coming in, a `GtkDropTarget` on the view accepts file lists and strings:
  the enter prefetches the payload bytes over the `GdkDrop` (capped at
  32 MiB), the compositor introduces them as a `wl_data_offer`, the page
  negotiates and reads them, and the verdict comes back on the synthetic
  drop (`docs/features/drag-loop.md` §7). Both directions passed a hand drag (text
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
  `seat-selftest --second-window` (`docs/features/multi-browser.md`). One path is still
  outside that guarantee: a drag **started** in an independent window. The
  out-of-bounds handover creates its `GdkDrag` on the demo's main toplevel
  (`docs/features/drag-loop.md` §6), so such a drag leaves from the wrong surface.
- **A tab-mode browser cannot be closed, and the request is dropped in
  silence.** `chromium_session_close_browser()` on a windowed browser runs the
  whole measured sequence (`browser N closing` → `browser N closed` →
  `browser 1 selected`); on a tab the helper detaches the view and calls
  `CloseBrowser`, and nothing comes back — no `OnBeforeClose`, so no
  `::browser-closed` either. An id that does not exist answers
  `status: no-such-browser`, which is how a host tells "already gone" from
  "silently refused" (code-review-2026-10-22.md M22; the three-way probe is in
  `docs/features/multi-browser.md` §1 and `docs/guides/minimal-host.md` §5). The same asymmetry
  shows up one signal earlier: `::popup-requested` carries only the URL, so a
  host cannot see whether the page asked for a background tab or a foreground
  window — that lives on the socket event's `disposition`, which the signal
  does not forward.
- **H.264/MP4 is absent from this CEF distribution.** Real YouTube reports
  `videoReady=4` but `canPlayType('video/mp4; codecs="avc1.42E01E"')` is empty;
  VP9 WebM answers `probably`, and YouTube still plays through VP9. MP4/H.264-only
  pages will fail until the distribution gets a codec decision. Measured in
  `docs/testing/real-sites.md` §3.1.
- **No pinch zoom and no tablet input.** Chromium binds `zwp_pointer_gestures`
  and `zwp_tablet_manager_v2` against real compositors; the nested compositor
  does not advertise them, so the page loses exactly those features and nothing
  else (`docs/internals/protocol-ceilings.md`). Primary selection (middle-click paste) is
  bridged separately and text-only (`docs/features/clipboard-bridge.md` §5).
- **A failed navigation reports twice, and `::load-end` still fires.** First the
  real code (`ERR_NAME_NOT_RESOLVED`), then `ERR_ABORTED` (-3) about a second
  later; the load-end arrives with HTTP code 0. Nothing is filtered, because
  picking which error a host should see is not this layer's call
  (`docs/guides/control-channel.md`).
- **Only the renderer's death is reported, and only how it died.** `::renderer-exited`
  comes from CEF's render-process callback — `kill -9` measured as `status=1` with
  error code 9, which is the OS-level wait status and nothing more: the minimal CEF
  distribution ships no crash handler, and no process in this tree carries a
  `--crashpad-handler-pid`, so there is no minidump and no stack behind it
  (`docs/internals/sandbox-policy.md` §8). When one of the other sandboxed children goes, the
  host hears nothing until the page stops answering.
- **A download can be accepted, refused, cancelled or paused/resumed.**
  `CefDownloadItemCallback::Pause/Resume` are wired as `download-pause` /
  `download-resume`; the terminal event is still CEF's `download-end`. Of the
  three end states, `interrupted` is the one `test/download-gate.sh` cannot
  reach: the local server keeps answering until Chromium gives up on its own
  (`docs/features/download-path.md`). And `complete` is not the moment the file appears:
  when the bytes arrived while the gate was still parked, the file is on disk as
  soon as the path is named but CEF's terminal event follows 3–6 s later, batched
  with Chromium's download-database flush (measured 3.24 / 4.12 / 5.72 s, so the
  gate waits 9 s — `docs/features/download-path.md` §4.4).
- **Find walks matches one at a time.** CEF's `Find` has no "jump to match N",
  and a brand-new search selects nothing at all (the active ordinal is 0), so a
  host that wants the fifth match sends five steps. Measured once in three runs,
  the first step reported the third match instead — the ordinal is Blink's own
  ordering and the gate deliberately does not pin it (`docs/features/find-path.md`).
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
  the handler twice (`docs/features/permission-prompt.md`).
- **The page's sound bypasses the widget entirely.** Audio lives in an
  unsandboxed `audio.mojom.AudioService` utility and reaches the host's speakers
  as an ordinary desktop app; the nested compositor never sees it, and GNOME's
  mixer cannot tell a muted stream from a quiet one. `set_muted` is the only
  switch, and nothing announces that a page *started* making noise
  (`docs/features/audio-path.md`).
- **The renderer is sandboxed; the helper itself is not, and cannot be.** The
  browser process holds the Wayland connection, the GPU buffers and the control
  socket, so it stays unsandboxed by design — the trust boundary is the page
  code in the renderer, not `cefnest-helper`. On a host with neither
  unprivileged user namespaces nor a setuid `chrome-sandbox` the helper dies at
  startup instead of quietly running unsandboxed; `CHROMIUM_HELPER_NO_SANDBOX=1`
  is the way to say it on purpose (`docs/internals/sandbox-policy.md`).
- **Native Wayland only, by design.** Under an X11 session GDK cannot give the
  widget a Wayland surface to hand to clients, so the component will not work
  there.
- The API is not stable. It *is* installed now — `src/meson.build` builds
  `libcefnest.so.0.0.1` with a `cefnest.pc` next to it — but nothing waits for a
  soname bump: a signal can gain a parameter (as `::closed` did) and a header can
  change shape between releases, so a host links against a build it built itself.
