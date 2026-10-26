#!/usr/bin/env bash
#
# test/cdp-gate.sh — the DevTools protocol pipe, on the real helper.
#
#   refuse     without cdp-enable the execute is refused by name: the pipe
#              exists only for a host that opened it
#   evaluate   Runtime.evaluate answers with the same value the js command
#              reads from the same page — one fact, two roads, equal answers
#   event      the protocol's own events arrive once the observer is on
#   screenshot Page.captureScreenshot comes back a base64 PNG
#   too-large  a 5 MB string result is refused as cdp-too-large instead of
#              going out as a line the channel could never carry
#
#   ./test/cdp-gate.sh
#   ./test/ci-session.sh ./test/cdp-gate.sh
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
    grep -nE 'demo: cdp|demo: script|helper: cdp|status' "$2" | tail -12 | sed 's/^/         /'
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

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$ROOT/test" \
  >"$WORK/http.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done

PAGE="http://127.0.0.1:$PORT/browser-data-page.html"

run_demo() { # run_demo <tag> <script-file>
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- refuse -----------------------------------------------------------------

cat >"$WORK/refuse.session" <<EOF
load $PAGE
wait 2000
cdp-execute 1 Runtime.evaluate {"expression":"1+1","returnByValue":true}
wait 1500
quit
EOF

echo "== cdp over $PAGE"
run_demo refuse "$WORK/refuse.session"
log=$WORK/refuse.log
if [ "$?" = 0 ]; then ok "refuse: demo exited 0"
else bad "refuse: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'helper status: cdp-not-enabled' "$log" refuse

# --- evaluate ----------------------------------------------------------------

cat >"$WORK/evaluate.session" <<EOF
load $PAGE
wait 2000
cdp-enable
wait 500
cdp-execute 1 Runtime.evaluate {"expression":"2+3","returnByValue":true}
wait 1500
js 2+3
wait 800
quit
EOF

run_demo evaluate "$WORK/evaluate.session"
log=$WORK/evaluate.log
if [ "$?" = 0 ]; then ok "evaluate: demo exited 0"
else bad "evaluate: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: cdp-result id=1 ok=1 message= .*"value":5[},]' "$log" evaluate
# One fact, two roads: the js command reads the same page and must agree.
expect 'demo: script [0-9]+ ok value=5' "$log" evaluate
expect 'helper: cdp-execute Runtime.evaluate host_id=1' "$log" evaluate

# --- events ------------------------------------------------------------------

# Protocol events need a domain enabled before they flow; Page.enable then a
# navigation produces frames the observer can only see because cdp-enable
# registered it.
cat >"$WORK/events.session" <<EOF
load $PAGE
wait 2000
cdp-enable
wait 500
cdp-execute 1 Page.enable {}
wait 1500
load $PAGE
wait 2500
quit
EOF

run_demo events "$WORK/events.session"
log=$WORK/events.log
if [ "$?" = 0 ]; then ok "events: demo exited 0"
else bad "events: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
# The enable itself and the page's life produce protocol traffic; the gate
# wants the channel demonstrably flowing, not a specific event name.
expect 'demo: cdp-event browser=1 method=[A-Za-z.]+' "$log" events

# --- screenshot --------------------------------------------------------------

cat >"$WORK/screenshot.session" <<EOF
load $PAGE
wait 2000
cdp-enable
wait 500
cdp-execute 2 Page.captureScreenshot {}
wait 2500
quit
EOF

run_demo screenshot "$WORK/screenshot.session"
log=$WORK/screenshot.log
if [ "$?" = 0 ]; then ok "screenshot: demo exited 0"
else bad "screenshot: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
# A PNG base64 always opens with iVBORw0KGgo — the JSON carries the data URL
# of the captured frame.
expect 'demo: cdp-result id=2 ok=1 message= json=.*iVBORw0KGgo' "$log" screenshot

# --- too large ---------------------------------------------------------------

cat >"$WORK/toolarge.session" <<EOF
load $PAGE
wait 2000
cdp-enable
wait 500
cdp-execute 3 Runtime.evaluate {"expression":"\\"x\\".repeat(5000000)","returnByValue":true}
wait 2500
quit
EOF

run_demo toolarge "$WORK/toolarge.session"
log=$WORK/toolarge.log
if [ "$?" = 0 ]; then ok "toolarge: demo exited 0"
else bad "toolarge: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: cdp-result id=3 ok=1 message=cdp-too-large' "$log" toolarge

if [ "$fails" = 0 ]; then printf 'cdp gate: passed\n'
else printf 'cdp gate: %d check(s) failed\n' "$fails"; exit 1; fi
