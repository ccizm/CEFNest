#!/usr/bin/env bash
#
# test/certificate-gate.sh — the certificate question, asked for real.
#
# A local HTTPS server with a self-signed certificate gives the gate a
# certificate Chromium genuinely refuses (ERR_CERT_AUTHORITY_INVALID), so
# every scenario below runs the parked flow end to end on the real helper:
#
#   allow    the parked question is answered allow, the page loads, and its
#            title proves the navigation completed past the error
#   deny     a fresh run denies, and the load dies with the error the page
#            shows
#   authorize  the fingerprint taken from the first run's log goes in ahead
#            of the load; the helper continues it without asking, and no
#            certificate-error is parked at all
#
# The parked ids are stable because each run starts a fresh helper and parks
# exactly one question before answering it: id 1.
#
#   ./test/certificate-gate.sh
#   ./test/ci-session.sh ./test/certificate-gate.sh
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
  if [ "$KEEP" = 1 ]; then
    echo "  note: transcripts kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: certificate|demo: script|load-error|helper: certificate' "$2" | tail -8 | sed 's/^/         /'
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
                b"<title>TLS-OK-PAGE</title></head>"
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

URL="https://127.0.0.1:$PORT/"

run_demo() { # run_demo <tag> <script-file>
  # CEFNEST_CERTIFICATE=ask is the demo taking the question so the script's
  # certificate-respond step can answer it; without it the session's default
  # deny would fire long before the step ran.
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CEFNEST_CERTIFICATE=ask \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- allow ------------------------------------------------------------------

cat >"$WORK/allow.session" <<EOF
load $URL
wait 2500
certificate-respond 1 allow
wait 2500
js document.title
wait 800
quit
EOF

echo "== certificate over https://127.0.0.1:$PORT/"
run_demo allow "$WORK/allow.session"
log=$WORK/allow.log
status=$?
if [ "$status" = 0 ]; then ok "allow: demo exited 0"
else bad "allow: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: certificate-error id=1 browser=1 error=ERR_CERT_[A-Z_]+ url='"$URL"' .* fingerprint=[0-9a-f]{64}' "$log" allow
expect 'demo: script [0-9]+ ok value="TLS-OK-PAGE"' "$log" allow

FP=$(grep -o 'fingerprint=[0-9a-f]\{64\}' "$log" | head -1 | cut -d= -f2)
if [ -n "$FP" ]; then ok "allow: fingerprint $FP"
else bad "allow: no fingerprint in the log"; fi

# --- authorize --------------------------------------------------------------

if [ -n "$FP" ]; then
  cat >"$WORK/auth.session" <<EOF
certificate-authorize $FP
load $URL
wait 3500
js document.title
wait 800
quit
EOF

  run_demo authorize "$WORK/auth.session"
  log=$WORK/authorize.log
  if [ "$?" = 0 ]; then ok "authorize: demo exited 0"
  else bad "authorize: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
  expect "helper: certificate-error $FP authorized" "$log" authorize
  expect 'demo: script [0-9]+ ok value="TLS-OK-PAGE"' "$log" authorize
  expect_no 'demo: certificate-error id=' "$log" authorize
else
  bad "authorize: skipped, no fingerprint came out of the allow run"
fi

# --- deny -------------------------------------------------------------------

cat >"$WORK/deny.session" <<EOF
load $URL
wait 2500
certificate-respond 1 deny
wait 2500
quit
EOF

run_demo deny "$WORK/deny.session"
log=$WORK/deny.log
if [ "$?" = 0 ]; then ok "deny: demo exited 0"
else bad "deny: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: certificate-error id=1 browser=1 error=ERR_CERT_' "$log" deny
# The deny lands in Chromium as the load error the page would show.
expect 'load error .*ERR_CERT' "$log" deny
expect_no 'value="TLS-OK-PAGE"' "$log" deny

# --- revoke -----------------------------------------------------------------
# One run proves both halves of the exemption's life: an authorized
# fingerprint continues without asking, and certificate-clear undoes it, so
# the same certificate asks again. (Each run_demo is a fresh helper, which is
# why both halves share one session.)

if [ -n "$FP" ]; then
  cat >"$WORK/revoke.session" <<EOF
certificate-authorize $FP
load $URL
wait 3500
certificate-clear
load $URL
wait 2500
certificate-respond 1 deny
wait 2500
quit
EOF

  run_demo revoke "$WORK/revoke.session"
  log=$WORK/revoke.log
  if [ "$?" = 0 ]; then ok "revoke: demo exited 0"
  else bad "revoke: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
  expect "helper: certificate-error $FP authorized" "$log" revoke
  expect 'helper: certificate-clear-authorized \([1-9][0-9]* forgotten\)' "$log" revoke
  expect 'demo: certificate-error id=1 browser=1 error=ERR_CERT_' "$log" revoke
  expect_no 'value="TLS-OK-PAGE"' "$log" revoke
else
  bad "revoke: skipped, no fingerprint came out of the allow run"
fi

if [ "$fails" = 0 ]; then printf 'certificate gate: passed\n'
else printf 'certificate gate: %d check(s) failed\n' "$fails"; exit 1; fi
