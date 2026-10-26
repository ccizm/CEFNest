#!/usr/bin/env bash
#
# test/console-gate.sh — the page's console lines and hover text reach the host.
#
# The probe page writes one console.info and one console.error as it loads;
# the gate asserts both sides of the relay (the helper's log line and the
# demo's ::console signal line) with the levels named. A second round hovers
# a link: ::status-message must arrive naming the link's URL and then again
# empty when the pointer moves off it — the address-bar-tail use case, proven
# end to end. (Measured 2026-10-26: CEF 156's views layer never calls
# OnTooltip in this nested Wayland setup, so ::tooltip-changed has no
# gate-able trigger; the wiring is live, the measurement is recorded in the
# plan.)
#
#   ./test/console-gate.sh
#   ./test/ci-session.sh ./test/console-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/console-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'console|status-message|demo:|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/console.session" <<EOF
load $PAGE
wait 1500
hover 40 40
wait 400
hover 61 92
wait 600
hover 40 40
wait 600
quit
EOF

echo "== console relay + status-message on hover"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/console.session" \
  "$DEMO" >"$WORK/console.log" 2>&1
status=$?
log=$WORK/console.log
if [ "$status" = 0 ]; then ok "console: demo exited 0"
else bad "console: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'helper: console info from file://.*console-page\.html:[0-9]+' "$log" helper-info
expect 'helper: console error from file://.*console-page\.html:[0-9]+' "$log" helper-error
expect 'demo: console browser=1 level=info .*message=CEFNEST-CONSOLE-INFO-LINE' "$log" signal-info
expect 'demo: console browser=1 level=error .*message=CEFNEST-CONSOLE-ERROR-LINE' "$log" signal-error
expect 'demo: status-message browser=1 text=file://.*console-page\.html#nowhere' "$log" hover-link
expect 'demo: status-message browser=1 text=$' "$log" hover-empty

echo
if [ $fails = 0 ]; then printf 'console gate: all checks passed\n'
else printf 'console gate: %d check(s) failed\n' "$fails"; exit 1; fi
