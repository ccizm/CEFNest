#!/usr/bin/env bash
#
# test/intercept-gate.sh — the consultative resource interception, for real.
#
# A local HTTP server serves a probe page whose image source arrives through
# the query string, so every scenario is one page and one rule:
#
#   cancel    a rule on /blocked/* parks the image request; request-respond
#             cancel answers it, and the page's onerror carries the verdict
#   redirect  the same rule; the answer redirects to a real image on the
#            same server, and the page sees that image load
#   passive   the mode switch: the event still arrives, nothing answers, and
#            the image loads anyway
#
# The request ids are stable the same way the certificate gate's are: each
# run starts a fresh helper, and only the probe image ever matches a rule,
# so the first parked question is request_id 1.
#
#   ./test/intercept-gate.sh
#   ./test/ci-session.sh ./test/intercept-gate.sh
#
# Needs python3; 77 without it.

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
    grep -nE 'demo: request|demo: script|demo: intercept|helper: request-intercept|helper: intercept' "$2" | tail -10 | sed 's/^/         /'
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
if ! command -v python3 >/dev/null; then echo "no python3 — the probe origin needs an HTTP server"; exit 77; fi

# The gate serves its own directory: the probe page copied in, plus the
# spare image the redirect and passive scenarios load — four green pixels,
# generated here rather than shipped as a checked-in binary.
cp "$ROOT/test/intercept-page.html" "$WORK/intercept-page.html"
python3 - "$WORK/intercept-spare.png" <<'PYEOF'
import zlib, struct, sys
side = 4
raw = b""
for _ in range(side):
    raw += b"\x00" + bytes([46, 194, 126] * side)
def chunk(tag, data):
    part = struct.pack(">I", len(data)) + tag + data
    return part + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw))
       + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
PYEOF

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WORK" \
  >"$WORK/http.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done

ORIGIN="http://127.0.0.1:$PORT"
PAGE="$ORIGIN/intercept-page.html"
SPARE="$ORIGIN/intercept-spare.png"

run_demo() { # run_demo <tag> <script-file>
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- cancel -----------------------------------------------------------------

cat >"$WORK/cancel.session" <<EOF
intercept-add 1 *://*/blocked/*
load $PAGE?src=/blocked/red.png
wait 1500
request-respond 1 cancel
wait 2500
js document.title
wait 800
quit
EOF

echo "== intercept over $ORIGIN"
run_demo cancel "$WORK/cancel.session"
log=$WORK/cancel.log
if [ "$?" = 0 ]; then ok "cancel: demo exited 0"
else bad "cancel: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: request-intercept id=1 browser=1 type=image main=0 method=GET url='"$ORIGIN"'/blocked/red.png' "$log" cancel
expect 'demo: request 1 cancel' "$log" cancel
expect 'demo: script [0-9]+ ok value="IMG-ERROR"' "$log" cancel

# --- redirect ---------------------------------------------------------------

cat >"$WORK/redirect.session" <<EOF
intercept-add 1 *://*/blocked/*
load $PAGE?src=/blocked/red.png
wait 1500
request-respond 1 redirect $SPARE
wait 2500
js document.title
wait 800
quit
EOF

run_demo redirect "$WORK/redirect.session"
log=$WORK/redirect.log
if [ "$?" = 0 ]; then ok "redirect: demo exited 0"
else bad "redirect: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: request-intercept id=1 .*type=image' "$log" redirect
expect 'demo: request 1 redirect' "$log" redirect
expect 'demo: script [0-9]+ ok value="IMG-LOADED:[0-9]+"' "$log" redirect

# --- passive ----------------------------------------------------------------

cat >"$WORK/passive.session" <<EOF
intercept-add 1 *://*/intercept-spare.png
intercept-mode passive
load $PAGE?src=/intercept-spare.png
wait 3500
js document.title
wait 800
quit
EOF

run_demo passive "$WORK/passive.session"
log=$WORK/passive.log
if [ "$?" = 0 ]; then ok "passive: demo exited 0"
else bad "passive: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'helper: intercept-mode passive' "$log" passive
expect 'demo: request-intercept id=1 .*url='"$ORIGIN"'/intercept-spare.png' "$log" passive
# Observed, not obstructed: nobody answers, and the image still loads.
expect 'demo: script [0-9]+ ok value="IMG-LOADED:[0-9]+"' "$log" passive
expect_no 'demo: request [0-9]+ ' "$log" passive

if [ "$fails" = 0 ]; then printf 'intercept gate: passed\n'
else printf 'intercept gate: %d check(s) failed\n' "$fails"; exit 1; fi
