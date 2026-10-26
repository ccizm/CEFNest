#!/usr/bin/env bash
#
# test/proxy-gate.sh — asserts that the proxy property reaches Chromium.
#
# Two runs against the same non-resolvable URL:
#
#   proxied   CEFNEST_PROXY names a local HTTP proxy that answers the request
#             itself; the page must load and carry the proxy's title
#   direct    no proxy is named; the same URL must fail with
#             ERR_NAME_NOT_RESOLVED, because proxy-probe.invalid has no DNS
#
# The host name is deliberately outside the loopback bypass rules, and the
# proxy logs the absolute-form request line as the second witness: a page that
# somehow loaded without it would fail that assertion.
#
#   ./test/proxy-gate.sh
#   ./test/ci-session.sh ./test/proxy-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
proxy_pid=
cleanup() {
  [ -n "$proxy_pid" ] && kill "$proxy_pid" 2>/dev/null
  if [ "$KEEP" = 1 ]; then echo "  note: transcripts kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'proxy|loaded|load error|script [0-9]+' "$2" | head -10 | sed 's/^/         /'
  fi
}

js_value() { # js_value <logfile> <n>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 to run the probe proxy"; exit 77; fi

# A minimal HTTP proxy: any request gets the same small page, except favicon.
# Enough for Chromium's absolute-form GET, and it records each request line so
# the gate can prove the traffic went through it.
cat > "$WORK/proxy.py" <<'PYEOF'
import socket, sys, threading

port = int(sys.argv[1])
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(16)
body = b"<!doctype html><title>PROXY-OK</title><h1 id=out>proxied</h1>"

def serve(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            break
        data += chunk
    line = data.split(b"\r\n", 1)[0].decode("latin1", "replace")
    sys.stderr.write("PROXY SAW " + line + "\n")
    sys.stderr.flush()
    if "/favicon.ico" in line:
        resp = b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    else:
        resp = (b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: close\r\n\r\n" + body)
    c.sendall(resp)
    c.close()

while True:
    conn, _ = srv.accept()
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
PYEOF

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
python3 "$WORK/proxy.py" "$PORT" >"$WORK/proxy.log" 2>&1 &
proxy_pid=$!
sleep 0.4

cat > "$WORK/proxy.session" <<'EOF'
load http://proxy-probe.invalid/
wait 3500
js document.title
wait 500
close
EOF

echo "== proxied"
timeout --kill-after=10s 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CEFNEST_PROXY="http://127.0.0.1:$PORT" \
  CHROMIUM_SESSION_SCRIPT="$WORK/proxy.session" \
  "$DEMO" >"$WORK/proxied.log" 2>&1
status=$?
if [ "$status" = 0 ]; then ok "proxied: demo exited 0"
else bad "proxied: demo exited $status"; tail -8 "$WORK/proxied.log" | sed 's/^/         /'; fi
expect "helper: --proxy-server=http://127\.0\.0\.1:$PORT" "$WORK/proxied.log" proxied
expect 'demo: loaded http://proxy-probe\.invalid/ \(HTTP 200\)' "$WORK/proxied.log" proxied
[ "$(js_value "$WORK/proxied.log" 1)" = "PROXY-OK" ] \
  && ok "proxied: the page came from the proxy" \
  || bad "proxied: title was '$(js_value "$WORK/proxied.log" 1)'"
expect "PROXY SAW GET http://proxy-probe\.invalid/ HTTP/1\.1" "$WORK/proxy.log" proxied

echo "== direct"
timeout --kill-after=10s 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" \
  CHROMIUM_SESSION_SCRIPT="$WORK/proxy.session" \
  "$DEMO" >"$WORK/direct.log" 2>&1
status=$?
if [ "$status" = 0 ]; then ok "direct: demo exited 0"
else bad "direct: demo exited $status"; tail -8 "$WORK/direct.log" | sed 's/^/         /'; fi
expect 'helper: load error -105 for http://proxy-probe\.invalid/' "$WORK/direct.log" direct
if grep -q 'helper: --proxy-server' "$WORK/direct.log"; then
  bad "direct: the helper still carried --proxy-server"
else ok "direct: no proxy switch was passed"; fi
if [ "$(js_value "$WORK/direct.log" 1)" = "PROXY-OK" ]; then
  bad "direct: the page loaded without a proxy"
else ok "direct: the page did not load"; fi

echo
if [ $fails = 0 ]; then printf 'proxy gate: all checks passed\n'
else printf 'proxy gate: %d check(s) failed\n' "$fails"; exit 1; fi
