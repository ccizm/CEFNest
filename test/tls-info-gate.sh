#!/usr/bin/env bash
#
# test/tls-info-gate.sh — the certificate facts about a loaded page.
#
# The same self-signed server the certificate gate uses, read from the other
# side: before the host allows anything the page does not load, so the probe
# runs twice — once over plain http (security_state not "secure", no
# certificate fields), once over the self-signed https after
# certificate-authorize (insecure state on a loaded page, then the leaf's
# subject, issuer, fingerprint and validity window).
#
#   ./test/tls-info-gate.sh
#   ./test/ci-session.sh ./test/tls-info-gate.sh
#
# Needs python3 and openssl; 77 without them.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
SERVER=
KEEP=${KEEP_LOGS:-0}
cleanup() {
  [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null
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
    grep -nE 'tls-info|certificate|loaded' "$2" | tail -8 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs a TLS server"; exit 77; fi
if ! command -v openssl >/dev/null; then echo "no openssl — the gate needs a certificate"; exit 77; fi

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" \
  -out "$WORK/cert.pem" -days 2 -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1

cat >"$WORK/tls-server.py" <<'EOF'
import http.server
import ssl
import sys

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = (b"<!doctype html><html><head><meta charset='utf-8'>"
                b"<title>TLS-PROBE</title></head>"
                b"<body><h1>tls probe page</h1></body></html>")
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[2], sys.argv[3])
server = http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
EOF

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$WORK/tls-server.py" "$PORT" "$WORK/cert.pem" "$WORK/key.pem" \
  >"$WORK/tls.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done

FP=$(openssl x509 -in "$WORK/cert.pem" -noout -fingerprint -sha256 2>/dev/null |
     cut -d= -f2 | tr -d ':' | tr 'A-F' 'a-f')

run_demo() { # run_demo <tag> <script-file>
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- plain http: loaded, no certificate facts --------------------------------

cat >"$WORK/http.session" <<EOF
load http://127.0.0.1:1/
wait 1200
tls-info
wait 1500
quit
EOF

echo "== tls-info over plain http (unusable port, error page committed)"
run_demo http "$WORK/http.session"
log=$WORK/http.log
if [ "$?" = 0 ]; then ok "http: demo exited 0"
else bad "http: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: tls-info id=[0-9]+ browser=1 ok=1 state=[a-z_-]+ subject= issuer= fp=' "$log" http

# --- self-signed https, authorized: insecure state with the leaf's facts -----

cat >"$WORK/https.session" <<EOF
certificate-authorize $FP
load https://127.0.0.1:$PORT/
wait 2500
tls-info
wait 1200
tls-info
wait 1200
tls-info
wait 1500
quit
EOF

echo "== tls-info over authorized self-signed https://127.0.0.1:$PORT/"
run_demo https "$WORK/https.session"
log=$WORK/https.log
if [ "$?" = 0 ]; then ok "https: demo exited 0"
else bad "https: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi

# The page loaded past an overridden certificate error: Chromium calls that
# state insecure-broken, and the leaf it broke is 127.0.0.1's. The security
# state and the certificate state arrive on separate protocol events, so a
# read in the first moments can carry one without the other — the gate asks
# three times and asserts on whichever read caught both.
expect 'demo: tls-info id=[0-9]+ browser=1 ok=1 state=insecure' "$log" https: insecure state
expect 'demo: tls-info id=[0-9]+ browser=1 ok=1 state=insecure[a-z-]* subject=127.0.0.1 issuer=127.0.0.1 fp=[0-9a-f]{64}' "$log" https: leaf facts
expect 'demo: tls-info id=[0-9]+ browser=1 .* from=[0-9]+ to=[0-9]+' "$log" https: validity

echo
if [ $fails = 0 ]; then printf 'tls-info gate: passed\n'
else printf 'tls-info gate: %d check(s) failed\n' "$fails"; exit 1; fi
