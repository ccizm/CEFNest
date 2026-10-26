#!/usr/bin/env bash
#
# test/hang-gate.sh — the render-unresponsive wiring exists, and a healthy
# page stays silent under it.
#
# Measured 2026-10-26: CEF 156's hang monitor never fires in this component's
# Alloy-on-Wayland shape — a renderer spinning in JS for 90 s with a click
# delivered onto it produces no OnRenderProcessUnresponsive (three
# experiments, observation windows to 35 s past the click). The face stays
# wired — helper handler, parked table, event, session signal, respond
# command, browser-close cleanup — because it is CEF's documented contract
# and costs nothing, exactly like the client-cert pair whose mTLS trigger
# this environment cannot produce either. So this gate is the C.2 shape:
# source-level wiring assertions plus the one behavioral fact that is
# observable here — a healthy, clicked page raises no question and answers
# nothing.
#
#   ./test/hang-gate.sh
#   ./test/ci-session.sh ./test/hang-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/click-probe.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <file> <tag>
  if grep -qE "$1" "$2"; then ok "$3"; else
    bad "$3: wanted /$1/ in $2"; fi
}
expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: /$1/ must not appear"
    grep -nE "$1" "$2" | head -3 | sed 's/^/         /'
  else
    ok "$3: no /$1/"
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

echo "== wiring: helper overrides, event names, respond command, cleanup"
expect 'bool OnRenderProcessUnresponsive' "$ROOT/helper/src/main.cpp" helper-handler
expect 'void OnRenderProcessResponsive' "$ROOT/helper/src/main.cpp" helper-responsive
expect '"render-unresponsive"' "$ROOT/helper/src/main.cpp" helper-event
expect '"render-unresponsive-respond"' "$ROOT/helper/src/main.cpp" helper-command
expect 'g_hang_dialogs' "$ROOT/helper/src/main.cpp" helper-table
expect '"render-unresponsive-closed"' "$ROOT/helper/src/main.cpp" helper-cleanup
expect '"render-unresponsive"' "$ROOT/src/chromium-session.c" session-event
expect 'render-unresponsive-respond' "$ROOT/src/chromium-session.c" session-command
expect 'chromium_session_render_unresponsive_respond' "$ROOT/src/chromium-session.h" session-api
expect 'hang-respond ' "$ROOT/src/main.c" demo-step

echo "== healthy page: clicked, no question raised"
cat >"$WORK/quiet.session" <<EOF
load $PAGE
wait 2000
click 500 300
wait 1500
js document.title
wait 600
quit
EOF
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/quiet.session" \
  "$DEMO" >"$WORK/quiet.log" 2>&1
status=$?
log=$WORK/quiet.log
if [ "$status" = 0 ]; then ok "quiet: demo exited 0"
else bad "quiet: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: script [0-9]+ ok value="clicked"' "$log" "quiet: click landed"
expect_no 'render-unresponsive' "$log" "quiet: no unresponsive event"
expect_no 'render-responsive' "$log" "quiet: no responsive event"

echo
if [ $fails = 0 ]; then printf 'hang gate: all checks passed\n'
else printf 'hang gate: %d check(s) failed\n' "$fails"; exit 1; fi
