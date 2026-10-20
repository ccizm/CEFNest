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
