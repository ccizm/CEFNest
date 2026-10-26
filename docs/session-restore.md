# Session restore recipe

English | [简体中文](session-restore.zh-CN.md)

A browser shell's session restore — the tabs that come back when the window
reopens — needs nothing in the component: it is a recipe the host follows
with the APIs that already exist. The component holds zero session state by
design (a helper's profile is the only Chromium-side state, and the data
directory contract governs that); what follows is the host-side shape.

## What to save

Per window, when it closes (or periodically, as a crash guard):

1. The list of tabs, in order, with the active one marked.
2. Per tab: its current URL (from `::address` / `chromium_session_browsers_list()`)
   and its **back stack**: one `chromium_session_history(browser, id)` call,
   whose `::history-result` carries `current_index` and `entries[]` — index,
   URL, title, and which entry is current.

Store both as your own JSON. Nothing here outlives your process otherwise:
browser ids are CEF's and change on every `start()`.

## What to restore

On the next `start()`, for each saved tab in order:

1. `chromium_session_new_browser(url, windowed)` — an empty `url`
   (`about:blank` is what a fresh browser loads anyway) if you want the tab
   to appear without loading twice.
2. Wait for `::browser-created` to learn the new browser id, then
   `chromium_session_history()` on it to see where it stands.
3. Rebuild the back stack **backwards**: for each saved entry from the one
   just before the saved current index down to 0,
   `chromium_session_history_go(browser, index)`. `history_go` is the
   component's own pipe (CEF's public API can read navigation history but
   never jump in it), so no `cdp_enable` is involved; each jump lands as the
   usual `::load-started` / `::load-end` run.
4. `chromium_session_select_browser()` the saved active tab last.

Two facts make this exact rather than approximate:

- `history()` reports the entries **Chromium still holds**, which shrinks as
  the browser prunes far-away entries. Save what it reported; restore
  whatever of it `history_go` still accepts — an out-of-range index answers
  `history-go-failed` on the status channel and the stack just ends there.
- `history_go` has no receipt. Drive the restore from the `::load-end` /
  `::address` events of the browser being restored, one jump at a time, and
  it stays observable the same way every navigation is.

The full back stack is what makes this worth doing over plain URL restore:
a restored tab keeps its history menu, and Back/Forward work immediately.

## What not to expect

- Scroll positions, form data and per-tab zoom are Chromium-side state tied
  to the old browser objects; a restored tab starts at the top.
- Session cookies do not survive a helper restart (Chromium's own rule, data
  directory or not) — a restored tab that depended on one lands on its login
  page. That is the same behaviour a real browser has unless the site's
  cookie is persistent.
- `ephy-session`-style crash recovery differs only in when you save: the
  recipe above run on a timer instead of on close.
