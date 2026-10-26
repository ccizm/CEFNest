#!/usr/bin/env bash
#
# test/auth-gate.sh — the HTTP authentication question, asked for real.
#
# A local server that demands Basic auth gives the gate a 401 Chromium cannot
# answer alone, so every scenario runs the parked flow end to end:
#
#   accept   the parked auth-required is answered with the right credentials,
#            and the page's title proves the request went through
#   wrong    wrong credentials re-ask; the second question is cancelled and
#            the page never loads
#   cancel   the first question is cancelled straight away; the page never
#            loads
#   proxy    a forward proxy answers 407 first; is_proxy names it, and the
#            same credentials carry the request through the proxy
#
# A source-level assertion grades the sibling interface that cannot be
# exercised here: OnSelectClientCertificate needs an mTLS server this
# environment cannot stand up, so its wiring (helper handler → event →
# session answer command) is read out of the sources instead — the
# certificate-gate's precedent for a question the environment cannot ask.
#
#   ./test/auth-gate.sh
#   ./test/ci-session.sh ./test/auth-gate.sh
#
# Needs python3; 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
SERVER=
PROXY=
KEEP=${KEEP_LOGS:-0}
cleanup() {
  [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null
  [ -n "$PROXY" ] && kill "$PROXY" 2>/dev/null
  if [ "$KEEP" = 1 ]; then echo "  note: transcripts kept in $WORK"
  else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: auth|demo: script|demo: loaded|helper: auth|status' "$2" | tail -8 | sed 's/^/         /'
  fi
}
expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs the auth server"; exit 77; fi

# --- the C.2 injection assertion: wiring, read out of the sources ------------

echo "== client-certificate wiring (no mTLS server possible here)"
for want in \
  'bool OnSelectClientCertificate' \
  'client-cert-request' \
  'client-cert-respond' \
  'client-cert-closed'; do
  if grep -q "$want" helper/src/main.cpp src/chromium-session.c; then
    ok "wiring: /$want/ present"
  else
    bad "wiring: /$want/ missing from helper or session"
  fi
done
if grep -q 'chromium_session_client_cert_respond' src/chromium-session.h; then
  ok "wiring: session API declared"
else
  bad "wiring: chromium_session_client_cert_respond not in the header"
fi

# --- the basic-auth origin server --------------------------------------------

cat >"$WORK/auth-server.py" <<'EOF'
import base64
import http.server
import sys

GOOD = base64.b64encode(b"user:pass").decode()

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") == "Basic " + GOOD:
            body = (b"<!doctype html><html><head><meta charset='utf-8'>"
                    b"<title>AUTH-OK-PAGE</title></head>"
                    b"<body><h1>auth probe page</h1></body></html>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = b"no"
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="gate"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, *args):
        pass

server = http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
server.serve_forever()
EOF

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$WORK/auth-server.py" "$PORT" >"$WORK/server.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done
URL="http://127.0.0.1:$PORT/"

run_demo() { # run_demo <tag> <script-file> [extra env]
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CEFNEST_AUTH=ask \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" ${3:-} \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- accept ------------------------------------------------------------------

cat >"$WORK/accept.session" <<EOF
load $URL
wait 1500
auth-respond user pass
wait 2500
js document.title
wait 800
quit
EOF

echo "== basic auth over $URL"
run_demo accept "$WORK/accept.session"
log=$WORK/accept.log
if [ "$?" = 0 ]; then ok "accept: demo exited 0"
else bad "accept: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: auth-required id=1 browser=1 proxy=0 scheme=basic realm=gate origin='"$URL" "$log" accept
expect 'demo: script [0-9]+ ok value="AUTH-OK-PAGE"' "$log" accept
expect_no 'demo: auth-closed' "$log" accept

# --- wrong, then cancel ------------------------------------------------------

cat >"$WORK/wrong.session" <<EOF
load $URL
wait 1500
auth-respond user wrong
wait 2000
auth-cancel
wait 2500
quit
EOF

echo "== wrong password re-asks, then cancel"
run_demo wrong "$WORK/wrong.session"
log=$WORK/wrong.log
if [ "$?" = 0 ]; then ok "wrong: demo exited 0"
else bad "wrong: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: auth-required id=1' "$log" wrong
expect 'demo: auth-required id=2' "$log" wrong
expect 'helper: auth 2 cancelled' "$log" wrong
expect_no 'value="AUTH-OK-PAGE"' "$log" wrong

# --- cancel at once ----------------------------------------------------------

cat >"$WORK/cancel.session" <<EOF
load $URL
wait 1500
auth-cancel
wait 2000
quit
EOF

echo "== cancel at once"
run_demo cancel "$WORK/cancel.session"
log=$WORK/cancel.log
if [ "$?" = 0 ]; then ok "cancel: demo exited 0"
else bad "cancel: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: auth-required id=1' "$log" cancel
expect 'helper: auth 1 cancelled' "$log" cancel
expect_no 'value="AUTH-OK-PAGE"' "$log" cancel

# --- proxy 407 ---------------------------------------------------------------

cat >"$WORK/proxy-server.py" <<EOF
import base64
import socket
import sys
import threading
import urllib.request

GOOD = base64.b64encode(b"puser:ppass").decode()
UPSTREAM = "http://127.0.0.1:$PORT/"

def handle(conn):
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                return
            data += chunk
        text = data.decode("latin-1", "replace")
        if "Proxy-Authorization: Basic " + GOOD not in text:
            conn.sendall(
                b"HTTP/1.1 407 Proxy Authentication Required\r\n"
                b'Proxy-Authenticate: Basic realm="gate-proxy"\r\n'
                b"Content-Length: 0\r\n"
                b"Connection: close\r\n\r\n")
            return
        line = text.split("\r\n", 1)[0]
        parts = line.split(" ")
        url = parts[1] if len(parts) > 1 else UPSTREAM
        if not url.startswith("http"):
            url = UPSTREAM
        request = urllib.request.Request(url)
        # Whatever the client said to the origin travels on: the proxy only
        # polices its own 407, not the origin's 401.
        for header in text.split("\r\n")[1:]:
            if header.lower().startswith("authorization:"):
                request.add_header("Authorization",
                                   header.split(":", 1)[1].strip())
                break
        try:
            with urllib.request.urlopen(request, timeout=5) as up:
                body = up.read()
                status = up.status
                content_type = up.headers.get("Content-Type", "text/html")
                challenge = None
        except urllib.error.HTTPError as error:
            body = error.read()
            status = error.code
            content_type = error.headers.get("Content-Type", "text/html")
            challenge = error.headers.get("WWW-Authenticate")
        head = "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %d\r\n" % (
            status, content_type, len(body))
        if challenge:
            head += "WWW-Authenticate: %s\r\n" % challenge
        head += "Connection: close\r\n\r\n"
        conn.sendall(head.encode("latin-1") + body)
    except Exception:
        pass
    finally:
        conn.close()

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", int(sys.argv[1])))
listener.listen(16)
while True:
    conn, _ = listener.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
EOF

PPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$WORK/proxy-server.py" "$PPORT" >"$WORK/proxy.log" 2>&1 &
PROXY=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PPORT") 2>/dev/null && break
  sleep 0.1
done

cat >"$WORK/proxy.session" <<EOF
wait 800
load $URL
wait 2000
auth-respond puser ppass
wait 2000
auth-respond user pass
wait 2500
js document.title
wait 800
quit
EOF

echo "== proxy auth over http://127.0.0.1:$PPORT"
run_demo proxy "$WORK/proxy.session" \
  "CEFNEST_PROXY=http://127.0.0.1:$PPORT CEFNEST_PROXY_BYPASS=<-loopback>"
log=$WORK/proxy.log
if [ "$?" = 0 ]; then ok "proxy: demo exited 0"
else bad "proxy: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: auth-required id=[0-9]+ browser=1 proxy=1 scheme=basic realm=gate-proxy' "$log" proxy
expect 'demo: script [0-9]+ ok value="AUTH-OK-PAGE"' "$log" proxy

echo
if [ $fails = 0 ]; then printf 'auth gate: passed\n'
else printf 'auth gate: %d check(s) failed\n' "$fails"; exit 1; fi
