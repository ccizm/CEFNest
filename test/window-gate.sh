#!/usr/bin/env bash
#
# test/window-gate.sh — the window.open picture, as new-browser geometry.
#
# A windowed browser opened with geometry answers ::window-properties with
# the bounds the window actually got, and this helper's frameless reality:
# no toolbar, no menubar, no status bar, resizable. A fullscreen flip is the
# other end of the same event.
#
#   ./test/window-gate.sh
#   ./test/ci-session.sh ./test/window-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/pixel-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'window-properties|window created|browser created' "$2" | tail -8 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/window.session" <<EOF
load $PAGE
wait 1200
new-window-geom 40 30 500 400 $PAGE
wait 2500
close-browser 2
wait 800
close
EOF

echo "== windowed browser at 500x400+40+30"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/window.session" \
  "$DEMO" >"$WORK/window.log" 2>&1
status=$?
log=$WORK/window.log
if [ "$status" = 0 ]; then ok "window: demo exited 0"
else bad "window: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# The geometry is what the window reports, at creation.
expect 'demo: window-properties browser=2 x=40 y=30 w=500 h=400 toolbar=0 menubar=0 status=0 resizable=1 fullscreen=0' "$log" geometry
# menubar=no is not a promise this helper could keep otherwise: its windows
# are frameless, and the event says so per window.
expect 'demo: window-properties browser=1 .*toolbar=0 menubar=0 status=0' "$log" chrome-less

echo
if [ $fails = 0 ]; then printf 'window gate: all checks passed\n'
else printf 'window gate: %d check(s) failed\n' "$fails"; exit 1; fi
