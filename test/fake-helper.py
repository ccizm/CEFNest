#!/usr/bin/env python3
"""A stand-in for the CEF helper, so the session's start/stop lifecycle can be
driven without starting a browser.

The demo hands the helper two environment variables and expects one newline
delimited JSON object per line back, which is all this script speaks:

  CHROMIUM_HELPER_SOCKET  the socket to connect to
  FAKE_HELPER_QUIT_AFTER  if set, milliseconds after the first load at which
                          the fake browser closes itself

Run it as the helper:

  CHROMIUM_HELPER=$(pwd)/test/fake-helper.py \\
  CHROMIUM_SESSION_SCRIPT=$(pwd)/test/close.session \\
  builddir/src/chromium-demo

Every command is answered with the events a real helper would send for it, and
"load" gets an address plus a load-end so the toolbar still lights up.
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

while True:
    line = channel.readline()
    if not line:
        break
    try:
        command = json.loads(line)
    except ValueError:
        continue

    if command.get("cmd") != "load":
        continue

    url = command.get("url", "")
    send({"event": "address", "url": url})
    send({"event": "title", "title": "fake"})
    send({"event": "load-end", "url": url, "http_code": 200})

    if quit_after:
        loaded_at = time.monotonic()
        while time.monotonic() - loaded_at < quit_after:
            time.sleep(0.05)
        send({"event": "closed"})
        break
