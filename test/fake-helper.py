#!/usr/bin/env python3
"""A stand-in for the CEF helper, so the session's start/stop lifecycle can be
driven without starting a browser.

The demo hands the helper two environment variables and expects one newline
delimited JSON object per line back, which is all this script speaks:

  CHROMIUM_HELPER_SOCKET  the socket to connect to
  FAKE_HELPER_QUIT_AFTER  if set, milliseconds after the first load at which
                          the fake browser closes itself
  FAKE_HELPER_CRASH_AFTER if set, milliseconds after the first load at which the
                          fake reports its render process gone (and then keeps
                          the socket open, because that is what the real helper
                          does — the page stops answering, nothing else changes)

A URL containing "download" also starts a download, which is how the host's
download layer can be driven without a browser:

  http://download.invalid/fake?name=report.pdf&bytes=4096

  name=   the suggested name (default fake-download.bin)
  bytes=  the total length (default 4096)

The fake then behaves the way the real helper does about it: it asks, and waits
for download-accept or download-reject. Accepted files are written to exactly the
path the host named, one KiB per 50 ms tick, so the progress events have somewhere
to come from; a rejected or never-answered download leaves no file at all.

Find is answered against one fixed sentence — "cefnest fake helper page" — so a
gate can assert exact counts without a browser: "find e" is six matches, "find
PAGE" is one with match_case off and zero with it on, "find zzz" is zero. The
reply shape is the one measured against real Chromium in docs/find-path.md: every
request takes a fresh identifier, a request that finds something answers with one
or more counting updates and then the same numbers marked final, a request that
finds nothing answers once, and a brand-new search reports ordinal 0 until the
host steps to a match. find-stop sends nothing back.

A URL containing "menu" sends a context-menu event, which is how the host's menu
layer is driven without a browser. menu=<name> picks the model — "link", "edit",
"selection" and "empty" are the ones docs/context-menu.md measured against real
Chromium, "mixed" is invented to carry a check, a radio, a submenu and two labels
that GTK has to escape — and x= / y= place it. menu=auto also closes the request
itself about half a second later, which is the shape of CEF dismissing a menu the
host never answered. The real helper's two answers (context-menu-select and
context-menu-dismiss) have no reply at all, so the fake writes what it received on
stderr instead — the demo's stderr is shared, and that is the only way a gate can
see a command cross the wire. A select with nothing pending is answered with the
no-context-menu status the real helper sends; a dismiss with nothing pending is
silent, exactly as there.

A URL containing "permission" asks for one permission, which is how the host's
prompt layer is driven without a browser. perm=<name> picks which ask, out of the
set docs/permission-prompt.md measured — audio, video, both, geolocation,
notifications, clipboard, midi, storage, network, empty — and origin= names the
requesting origin. The two queues behave the way the real browser's do: a media
ask the host has not answered holds the queue, and an ask made behind it arrives
as its own request only once the slot is free again, while a prompt the host has
not answered is dismissed as "ignore" the moment the next prompt shows up. That is
where the replay stops: measured, a prompt the host sits on also makes the ask
made immediately after it vanish without ever reaching the helper
(docs/permission-prompt.md §3), and a host cannot answer that differently, so it
is not simulated here.
auto=1 closes the request itself about half a second later, which is the shape of
Chromium taking a prompt back. The one answer the real helper takes
(permission-respond, with accept, deny or dismiss) has no reply of its own, so
the fake writes what it received on stderr; an id that is already gone gets the
no-permission-request status the real helper sends.

Run it as the helper:

  CHROMIUM_HELPER=$(pwd)/test/fake-helper.py \\
  CHROMIUM_SESSION_SCRIPT=$(pwd)/test/close.session \\
  builddir/src/cefnest-demo

Every command is answered with the events a real helper would send for it, and
"load" gets an address plus a load-end so the toolbar still lights up. A URL whose
host ends in `.invalid` is answered the way a real failed navigation is: load-error,
then a title, then a load-end with HTTP code 0 and no address at all.
"""
import json
import os
import select
import socket
import sys
import time

path = os.environ.get("CHROMIUM_HELPER_SOCKET")
if not path:
    sys.exit("fake helper: no CHROMIUM_HELPER_SOCKET")

quit_after = float(os.environ.get("FAKE_HELPER_QUIT_AFTER", "0")) / 1000.0
crash_after = float(os.environ.get("FAKE_HELPER_CRASH_AFTER", "0")) / 1000.0

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(200):
    try:
        sock.connect(path)
        break
    except OSError:
        time.sleep(0.05)
else:
    sys.exit("fake helper: no compositor is listening at " + path)

channel = sock.makefile("rw")


def send(payload):
    channel.write(json.dumps(payload) + "\n")
    channel.flush()


send({"event": "ready", "pid": os.getpid(), "cef": "fake"})

muted = False

DOWNLOAD_CHUNK = 1024
DOWNLOAD_FILL = b"cefnest fake download payload\n"

# id -> the state of one download the fake is holding for the host to decide on.
downloads = {}
next_download_id = 1


def query(value, url, default):
    marker = url.find(value + "=")
    if marker < 0:
        return default
    tail = url[marker + len(value) + 1:]
    for stop in ("&", "#"):
        cut = tail.find(stop)
        if cut >= 0:
            tail = tail[:cut]
    return tail or default


def send_download_end(download_id, state, path, received):
    send({"event": "download-end", "id": download_id, "state": state,
          "path": path, "received_bytes": received})


def begin_download(url):
    global next_download_id

    download_id = next_download_id
    next_download_id += 1
    total = int(query("bytes", url, "4096"))
    name = query("name", url, "fake-download.bin")

    downloads[download_id] = {"state": "pending", "path": None, "handle": None,
                              "total": total, "received": 0}
    # Same shape the real helper sends: an id, what the page called it, and the
    # length the server stated. Nothing is on disk yet.
    send({"event": "download-begin", "id": download_id, "url": url,
          "suggested_name": name, "mime_type": "application/octet-stream",
          "total_bytes": total})


def tick_downloads():
    for download_id, item in list(downloads.items()):
        if item["state"] != "running":
            continue

        remaining = item["total"] - item["received"]
        wanted = min(DOWNLOAD_CHUNK, remaining)
        chunk = (DOWNLOAD_FILL * (wanted // len(DOWNLOAD_FILL) + 1))[:wanted]
        if chunk:
            item["handle"].write(chunk)
            item["handle"].flush()
        item["received"] += len(chunk)

        total = item["total"]
        send({"event": "download-progress", "id": download_id,
              "received_bytes": item["received"], "total_bytes": total,
              "percent": 100 if total <= 0 else item["received"] * 100 // total,
              "speed": DOWNLOAD_CHUNK * 20})

        if item["received"] < total:
            continue

        item["handle"].close()
        item["handle"] = None
        send_download_end(download_id, "complete", item["path"], item["received"])
        del downloads[download_id]


# The page a scripted search runs against. The counts a gate asserts are
# occurrences in this text: "find e" is six because this string has six e's.
FAKE_PAGE_TEXT = "cefnest fake helper page"

# One search the fake is holding. CEF's own counter turns out to move per request
# — a step takes a fresh number just as a new search does — so every update about
# one request shares it.
find_state = {"identifier": 0, "matches": [], "ordinal": 0}


def fake_find(pattern, match_case, forward, step):
    # The shape is the one measured against real Chromium (docs/find-path.md):
    # every request — a new search and a step alike — gets a fresh identifier, a
    # request that finds something answers with at least one counting update and
    # then the same numbers marked final, a request that finds nothing answers
    # once, and a new search reports ordinal 0 because no match is the active one
    # until the host steps.
    find_state["identifier"] += 1
    before = find_state["ordinal"]

    if not step:
        haystack = FAKE_PAGE_TEXT if match_case else FAKE_PAGE_TEXT.lower()
        needle = pattern if match_case else pattern.lower()
        matches = []
        at = haystack.find(needle) if needle else -1
        while at >= 0:
            matches.append(at)
            at = haystack.find(needle, at + 1)
        find_state["matches"] = matches
        find_state["ordinal"] = 0
    elif find_state["matches"]:
        last = len(find_state["matches"])
        if forward:
            find_state["ordinal"] = (find_state["ordinal"] + 1
                                     if find_state["ordinal"] < last else 1)
        else:
            find_state["ordinal"] = (find_state["ordinal"] - 1
                                     if find_state["ordinal"] > 1 else last)

    def update(is_final, ordinal=None):
        send({"event": "find-result", "identifier": find_state["identifier"],
              "count": len(find_state["matches"]),
              "active_ordinal": (find_state["ordinal"] if ordinal is None
                                 else ordinal),
              "final": is_final})

    # Measured once, on a previous-step taken straight after a new search: real
    # Chromium reported the not-yet-active ordinal 0 first and only then the
    # wrapped-to-the-end one, so that request answered three times. A host that
    # assumed two updates would be wrong, so the fake can do it too.
    extra_counting = (step and not forward and before == 0
                      and bool(find_state["matches"]))

    if find_state["matches"]:
        if extra_counting:
            update(False, 0)
        update(False)
    update(True)


# The models docs/context-menu.md measured out of a real CEF 154, spelled the way
# they arrive on the channel: a separator carries no label and is always enabled,
# and the labels keep the "(&B)" mnemonic spelling CEF uses.
def menu_item(item_id, label, enabled=True, type="command", checked=False):
    return {"id": item_id, "label": label, "type": type, "enabled": enabled,
            "checked": checked}


MENU_SEP = menu_item(-1, "", type="separator")

MENU_MODELS = {
    # A link, an image and a plain page all answer this same model: CEF's default
    # has nothing link- or image-specific in it.
    "link": [menu_item(100, "返回(&B)", False), menu_item(101, "前进(&F)", False),
             MENU_SEP, menu_item(131, "打印(&P)…"), menu_item(132, "查看网页源代码(&V)")],
    # An editable with the caret in it and nothing selected.
    "edit": [menu_item(110, "撤消(&U)", False), menu_item(111, "重做(&R)", False),
             MENU_SEP, menu_item(112, "剪切(&T)", False),
             menu_item(113, "复制(&C)", False), menu_item(114, "粘贴(&P)"),
             menu_item(115, "粘贴为纯文本"), MENU_SEP, menu_item(117, "全选(&A)")],
    # A selection in ordinary page text: copy, and nothing else.
    "selection": [menu_item(113, "复制(&C)")],
    # Not measured: the shapes the default model never shows but the protocol can
    # carry, including two labels GTK has to escape.
    "mixed": [menu_item(200, "拼写检查(&S)", type="check", checked=True),
              menu_item(201, "简体中文", type="radio", checked=True),
              menu_item(26500, "复制 & 粘贴"), menu_item(26501, "下划线_测试"),
              menu_item(26502, "", type="submenu")],
    "empty": [],
    # Separators only: no rows at all once they are turned into sections.
    "seps": [MENU_SEP, MENU_SEP],
}

# The link model again, for the run that wants the request closed over by the
# helper rather than answered by the host.
MENU_MODELS["auto"] = MENU_MODELS["link"]

# The one request the fake is holding for the host, and when it closes it itself.
menu = {"pending": False, "dismiss_at": None}


def begin_context_menu(url):
    model = query("menu", url, "link")
    items = MENU_MODELS.get(model)
    if items is None:
        send({"event": "status", "message": "unknown-menu-model"})
        return

    send({"event": "context-menu",
          "x": int(query("x", url, "140")),
          "y": int(query("y", url, "40")),
          "items": [dict(item) for item in items]})
    menu["pending"] = True
    # "auto" stands in for CEF closing a request the host never answered.
    menu["dismiss_at"] = time.monotonic() + 0.5 if model == "auto" else None


def tick_menu():
    if menu["dismiss_at"] and time.monotonic() >= menu["dismiss_at"]:
        menu["dismiss_at"] = None
        if not menu["pending"]:
            return
        menu["pending"] = False
        send({"event": "context-menu-dismissed"})


def log(text):
    print("fake: " + text, file=sys.stderr, flush=True)


# The asks docs/permission-prompt.md measured out of a real CEF 154, spelled the
# way they arrive on the channel: the two queues carry the two CEF callbacks, a
# media ask names capture bits and a prompt ask names permission types with their
# raw bit values.
PERM_ASKS = {
    "audio": ("media", ["device-audio"], 1),
    "video": ("media", ["device-video"], 2),
    "both": ("media", ["device-audio", "device-video"], 3),
    "geolocation": ("prompt", ["geolocation"], 256),
    "notifications": ("prompt", ["notifications"], 32768),
    "clipboard": ("prompt", ["clipboard"], 16),
    "midi": ("prompt", ["midi-sysex"], 8192),
    "storage": ("prompt", ["storage-access"], 1048576),
    "network": ("prompt", ["local-network", "loopback-network"], 201326592),
    "empty": ("prompt", [], 0),
}

# One request per queue that the fake is holding for the host to answer, plus the
# asks waiting behind it.
permissions = {"media": None, "prompt": None}
permission_queue = []
next_permission_id = 1


def send_permission(kind, names, raw, origin):
    global next_permission_id

    request = {"id": next_permission_id, "kind": kind, "names": names, "raw": raw,
               "dismiss_at": None}
    next_permission_id += 1
    permissions[kind] = request
    send({"event": "permission-request", "id": request["id"], "kind": kind,
          "origin": origin, "permissions": names, "raw": raw})


def begin_permission(url):
    picked = query("perm", url, "geolocation")
    ask = PERM_ASKS.get(picked)
    if ask is None:
        send({"event": "status", "message": "unknown-permission"})
        return

    kind, names, raw = ask
    origin = query("origin", url, "http://fake.invalid/")
    # "auto" stands in for Chromium closing a prompt the host never answered —
    # the page navigating, or the next ask pushing it out.
    auto = query("auto", url, "0") not in ("0", "")

    if permissions[kind]:
        if kind == "media":
            # Measured: a media ask the host has not answered is queued by
            # Chromium, and the ask made behind it arrives as its own request the
            # moment the slot is free again. Nothing is lost, and nothing arrives
            # early.
            permission_queue.append((kind, names, raw, origin, auto))
            return
        # Measured again: prompts do not queue. The one being held is dismissed as
        # "ignore" a moment before the new one shows up, and the host has to take
        # its own row down when it hears that.
        held = permissions[kind]
        permissions[kind] = None
        send({"event": "permission-dismissed", "id": held["id"], "result": "ignore"})

    send_permission(kind, names, raw, origin)
    if auto:
        permissions[kind]["dismiss_at"] = time.monotonic() + 0.5


def deliver_queued():
    """The asks Chromium was holding behind a slot that is now empty. One per
    queue: delivering the next fills the slot again."""
    for entry in list(permission_queue):
        kind = entry[0]
        if permissions[kind]:
            continue
        permission_queue.remove(entry)
        send_permission(kind, entry[1], entry[2], entry[3])
        if entry[4]:
            permissions[kind]["dismiss_at"] = time.monotonic() + 0.5


def tick_permissions():
    for kind, request in permissions.items():
        if not request or not request["dismiss_at"]:
            continue
        if time.monotonic() < request["dismiss_at"]:
            continue
        request["dismiss_at"] = None
        permissions[kind] = None
        send({"event": "permission-dismissed", "id": request["id"],
              "result": "ignore"})
        deliver_queued()


while True:
    # A 50 ms timeout is what lets an accepted download advance while the fake
    # waits for the next command: the real helper's ticks come from Chromium.
    ready, _, _ = select.select([sock], [], [], 0.05)
    if not ready:
        tick_downloads()
        tick_menu()
        tick_permissions()
        continue

    line = channel.readline()
    if not line:
        break
    try:
        command = json.loads(line)
    except ValueError:
        continue

    if command.get("cmd") == "mute":
        # Answered with the state the fake ends up in rather than with what was
        # asked, because the real helper reads CefBrowserHost back, and "on"
        # missing means toggle there too.
        wanted = command.get("on")
        muted = (not muted) if wanted is None else bool(wanted)
        send({"event": "muted", "muted": muted})
        continue

    if command.get("cmd") == "find":
        pattern = command.get("pattern", "")
        if not pattern:
            # The real helper refuses this rather than reading it as "stop",
            # so that find-stop stays the only way to end a search.
            send({"event": "status", "message": "find-needs-pattern"})
            continue
        fake_find(pattern, bool(command.get("match_case")),
                  bool(command.get("forward", True)), bool(command.get("next")))
        continue

    if command.get("cmd") == "find-stop":
        # Nothing comes back, which is what StopFinding is: the search ends and
        # no count is reported for it. What a step means *after* a stop was not
        # measured against the real browser, so the fake does not invent an
        # answer either — it drops the matches, and a gate must not ask.
        find_state["matches"] = []
        find_state["ordinal"] = 0
        continue

    if command.get("cmd") == "context-menu-select":
        if not menu["pending"]:
            # What the real helper says when the host answers a request that is
            # already gone; the callback slot is empty over there too.
            send({"event": "status", "message": "no-context-menu"})
            continue
        menu["pending"] = False
        # Continue() has no reply of its own, so the only trace a gate can assert
        # on is the fake saying what it was handed.
        log("context-menu-select id=%s flags=%s" %
            (command.get("id"), command.get("flags", 0)))
        continue

    if command.get("cmd") == "context-menu-dismiss":
        if not menu["pending"]:
            # Silent here because Cancel() on an empty slot is silent there.
            continue
        menu["pending"] = False
        log("context-menu-dismiss")
        continue

    if command.get("cmd") == "permission-respond":
        respond_id = command.get("id")
        held = None
        for request in permissions.values():
            if request and request["id"] == respond_id:
                held = request
                break

        if not held:
            # The real helper's answer for a request that is already gone: the
            # callback slot is empty, so nothing happens and only a status is left.
            send({"event": "status", "message": "no-permission-request"})
            continue

        permissions[held["kind"]] = None
        # Continue() and Cancel() have no reply of their own, so the trace a gate
        # can assert on is the fake saying what it was handed.
        log("permission-respond id=%s result=%s kind=%s names=%s" %
            (held["id"], command.get("result"), held["kind"],
             "+".join(held["names"])))
        deliver_queued()
        continue

    if command.get("cmd") == "download-accept":
        download_id = command.get("id")
        path = command.get("path") or ""
        item = downloads.get(download_id)

        if not item or item["state"] != "pending":
            send({"event": "status", "message": "no-such-download"})
            continue
        if not path:
            send({"event": "status", "message": "download-accept-needs-path"})
            continue

        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        item["handle"] = open(path, "wb")
        item["path"] = path
        item["state"] = "running"
        continue

    if command.get("cmd") == "download-reject":
        download_id = command.get("id")
        item = downloads.get(download_id)

        if not item:
            send({"event": "status", "message": "no-such-download"})
            continue
        if item["state"] == "pending":
            # The real helper drops the parked gate and closes this itself,
            # because no update follows a download that never started.
            send_download_end(download_id, "cancelled", "", 0)
            del downloads[download_id]
        elif item["state"] == "running":
            # What Chromium does to a cancelled file: the partial write goes.
            item["handle"].close()
            os.unlink(item["path"])
            send_download_end(download_id, "cancelled", "", item["received"])
            del downloads[download_id]
        continue

    if command.get("cmd") != "load":
        continue

    url = command.get("url", "")

    if ".invalid" in url:
        # Measured shape of a navigation that never commits: load-error, then the
        # error page's title, then a load-end with HTTP code 0 — and no address.
        host = url.split("//")[-1].split("/")[0].split(":")[0]
        send({"event": "load-error", "error_code": -105, "url": url,
              "message": "ERR_NAME_NOT_RESOLVED"})
        send({"event": "title", "title": host})
        send({"event": "load-end", "url": url, "http_code": 0})
        continue

    send({"event": "address", "url": url})
    send({"event": "title", "title": "fake"})
    send({"event": "load-end", "url": url, "http_code": 200})

    if "download" in url:
        begin_download(url)

    if "menu" in url:
        begin_context_menu(url)

    if "permission" in url:
        begin_permission(url)

    if crash_after:
        crashed_at = time.monotonic() + crash_after
        while time.monotonic() < crashed_at:
            time.sleep(0.05)
        # Stay alive: the real helper does, and keeps reporting state for a page
        # whose renderer is gone.
        send({"event": "renderer-exited", "status": 2, "error_code": 11,
              "message": "Aw, Snap!"})

    if quit_after:
        loaded_at = time.monotonic()
        while time.monotonic() - loaded_at < quit_after:
            time.sleep(0.05)
        send({"event": "closed"})
        break
