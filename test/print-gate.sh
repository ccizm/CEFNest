#!/usr/bin/env bash
#
# test/print-gate.sh — window.print() reaches the host, print-pdf closes the loop.
#
# The probe page calls window.print() once it has painted. The component runs
# no printer dialog — printing is the host's GTK transaction — so the ask may
# only arrive as ::print-requested. The second half drives the existing
# print-pdf step and checks the stream still closes with PDF bytes, which is
# the host's answer to the same ask.
#
#   ./test/print-gate.sh
#   ./test/ci-session.sh ./test/print-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/print-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'print|pdf|demo:|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/print.session" <<EOF
load $PAGE
wait 2500
print-pdf
wait 2500
quit
EOF

echo "== print-requested + print-pdf loop"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/print.session" \
  "$DEMO" >"$WORK/print.log" 2>&1
status=$?
log=$WORK/print.log
if [ "$status" = 0 ]; then ok "print: demo exited 0"
else bad "print: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'helper: print-requested browser=1' "$log" helper
expect 'demo: print-requested browser=1' "$log" signal
expect 'demo: pdf id=[0-9]+ bytes=[1-9][0-9]* truncated=0 error="" magic=1 eof=1' "$log" pdf-bytes

echo
if [ $fails = 0 ]; then printf 'print gate: all checks passed\n'
else printf 'print gate: %d check(s) failed\n' "$fails"; exit 1; fi
