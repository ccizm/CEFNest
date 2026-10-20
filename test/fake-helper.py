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

while True:
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
