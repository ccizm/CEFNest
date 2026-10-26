#!/usr/bin/env bash
#
# test/zoom-gate.sh — the zoom command family, zoom-set included.
#
# The helper answers every zoom command with the level it read back, so this
# gate grades the values Chromium holds, not values this side computed:
#
#   in       one CEF step up from what the page started with (0.50)
#   set      an arbitrary level (1.25), the command this batch completed
#   reset    back to 0.00
#   out      one step below the reset, which is -0.50
#
# Each command gets its own wait: the zoom is applied asynchronously and a
# command answered before the previous one settled would measure a moving
# target. The reload at the end asserts the load-time report still exists.
#
#   ./test/zoom-gate.sh
#   ./test/ci-session.sh ./test/zoom-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then echo "  note: transcripts kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'zoom|script [0-9]+' "$2" | head -12 | sed 's/^/         /'
  fi
}

# The n-th zoom level the demo reported, in order.
levels() {
  sed -n 's/^demo: zoom level=\(.*\)$/\1/p' "$1"
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat > "$WORK/zoom.session" <<'EOF'
load data:text/html,<title>zoom</title><h1>zoom</h1>
wait 2000
zoom-in
wait 800
zoom-set 1.25
wait 800
zoom-reset
wait 800
zoom-out
wait 800
reload
wait 1500
close
EOF

echo "== zoom"
timeout --kill-after=10s 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/zoom.session" \
  "$DEMO" >"$WORK/zoom.log" 2>&1
status=$?
log=$WORK/zoom.log
if [ "$status" = 0 ]; then ok "zoom: demo exited 0"
else bad "zoom: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# The first report is the load's; the four commands follow. Grading the whole
# sequence as one string keeps the order and the duplicates in the transcript
# from being mistaken for separate facts.
got=$(levels "$log" | tr '\n' ' ')
case "$got" in
  "0.00 0.50 1.25 0.00 -0.50 "*)
    ok "zoom: levels were $got" ;;
  *)
    bad "zoom: levels were '$got', wanted 0.00 0.50 1.25 0.00 -0.50 then the load report" ;;
esac

expect 'demo: loaded data:text/html,<title>zoom</title><h1>zoom</h1> \(HTTP 200\)' "$log" zoom

echo
if [ $fails = 0 ]; then printf 'zoom gate: all checks passed\n'
else printf 'zoom gate: %d check(s) failed\n' "$fails"; exit 1; fi
