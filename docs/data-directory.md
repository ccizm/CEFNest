# Data directory

English | [简体中文](data-directory.zh-CN.md)

Where the page's own data — cookies, localStorage, IndexedDB, the HTTP cache —
lives, and when any of it touches the disk. This is the complete contract
behind `chromium_session_set_data_directory()` /
`chromium_session_get_data_directory()` / `chromium_session_clear_data()` and
the `data-directory` property; the method and signal digest is in
[api.md](api.md).

**A session keeps nothing on disk unless you name a directory.** With no data
directory the helper runs as Chromium's incognito: cookies, localStorage,
IndexedDB and the HTTP cache live in memory, and the one install-level
directory CEF insists on writing is a mode-0700 temporary directory under
`$XDG_RUNTIME_DIR` (or `/dev/shm` when that is unset). It is removed when the
helper exits, and a root left behind by a killed helper is swept on the next
start. Nothing is ever written under `~/.cache`, and CEF is never allowed to
fall back to `~/.config/cef_user_data`.

To keep page data across restarts, name one absolute path before `start()`:

```c
chromium_session_set_data_directory (session, "/home/me/.local/share/myapp/cefnest");
chromium_session_start (session, "https://example.com", &error);
```

| Before `start()` | `root_cache_path` | `cache_path` | Result |
| --- | --- | --- | --- |
| nothing (default) | `$XDG_RUNTIME_DIR/cefnest-helper-<pid>-XXXXXX`, mode 0700 | empty | incognito, in-memory cache; the temporary directory goes on exit, leftovers are swept on the next start |
| `set_data_directory (session, P)` | `P` | `P/browser` | persistent profile: cookies and storage survive restarts |

Rules:

- The path must be absolute. `~` is not expanded, and a relative path makes
  `start()` fail with `G_IO_ERROR_INVALID_ARGUMENT`.
- The directory is created with mode 0700 when missing. CEFNest never deletes
  it on its own; `chromium_session_clear_data()` is the one eraser, and it
  keeps the property set (the next `start()` recreates the directory empty).
- One directory, one helper: a second session pointed at a directory already
  in use fails early. In the same process the registry returns
  `G_IO_ERROR_BUSY` from `start()`; across processes the helper's `flock`
  refuses it, so that session closes with "process exited".
- It is one complete profile. CEF requires `cache_path` to live below
  `root_cache_path`, so there is no "cache on disk, cookies in memory" split.
- Session cookies die with the helper even in persistent mode
  (`persist_session_cookies` is false).
- Setting the property while a session runs affects the next `start()`, not
  the running helper.

**Plugins live under this tree too.** The data directory holds
`<data-dir>/cefnest-plugins/` — the plugin **install root**: one subdirectory
per plugin package (the directory name is the plugin id), and a plugin's
`chrome.storage` data lands in `<data-dir>/cefnest-plugins/<id>/storage.json`.
The manager scans it before every `add_search_path()` directory, so the
user's own install wins a duplicate id over a path the host pointed at; a
directory that does not exist yet (a fresh profile has installed nothing) is
not an error — the scan is simply empty. With no data directory there is no
default plugin location at all: the "no directory means nothing on disk"
contract holds for plugins too, and plugin directories then come only from
explicit `add_search_path()` calls. Moving the property moves the root; the
model refreshes on the next `scan()`.

**Where to put it.** Use `g_get_user_data_dir()` — normally
`~/.local/share/<app>/…`. Do **not** use `~/.cache`: XDG defines it as
disposable, so a cleaner such as BleachBit can silently erase logins, and the
user sees "randomly logged out". Expect the platform, not CEFNest, to decide
whether data follows an uninstall:

| Install shape | What happens to the data directory |
| --- | --- |
| Flatpak | inside the sandbox the path resolves under `~/.var/app/<id>/…`; `flatpak uninstall --delete-data` removes it |
| Snap | removed by `snap remove --purge` |
| deb / rpm | user data survives uninstall, as it does for Chromium and Firefox; delete it yourself or call `clear_data()` |

The demo accepts `--data-dir=<absolute path>`; without it the demo is
incognito too.

A standalone helper uses the same contract as an environment variable:
`CHROMIUM_HELPER_DATA_DIR=/absolute/path ./cefnest-helper` is persistent,
unset is incognito. A host always decides for its child: the library unsets
`CHROMIUM_HELPER_DATA_DIR` before spawning, so a value inherited from the
host's environment cannot turn a session disk-backed.

Older builds derived a cache directory under `~/.cache/cefnest-helper*`.
Nothing writes there any more, and nothing deletes those directories for you;
remove them after upgrading:

```sh
rm -rf ~/.cache/cefnest-helper ~/.cache/cefnest-helper-hosted-*
```
