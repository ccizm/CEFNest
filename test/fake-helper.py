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


while True:
    # A 50 ms timeout is what lets an accepted download advance while the fake
    # waits for the next command: the real helper's ticks come from Chromium.
    ready, _, _ = select.select([sock], [], [], 0.05)
    if not ready:
        tick_downloads()
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
