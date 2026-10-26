#!/usr/bin/env bash
#
# test/browsers-gate.sh — the browser table, on the session.
#
# The helper pushes the whole table on every change and ::browsers-changed
# says so; chromium_session_browsers_list() reads the cache. Four triggers
# are asserted — create, close, select, title change — and the point of the
# whole feature: a background tab's address change must be readable from the
# list without ever selecting it.
#
#   ./test/browsers-gate.sh
#   ./test/ci-session.sh ./test/browsers-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
FIRST="file://$ROOT/test/browser-data-page.html"
SECOND="file://$ROOT/test/history-second-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'browser-row|browsers-changed|demo: (uri|loaded|browser)' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/table.session" <<EOF
load $FIRST
wait 1500
new-tab $SECOND
wait 2000
load-browser 2 $FIRST
wait 2000
select-browser 1
wait 800
close-browser 2
wait 1000
browsers
wait 500
close
EOF

echo "== browser table over two tabs"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/table.session" \
  "$DEMO" >"$WORK/table.log" 2>&1
status=$?
log=$WORK/table.log
if [ "$status" = 0 ]; then ok "table: demo exited 0"
else bad "table: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# Create: the second browser shows up with its own row (a new tab takes the
# active slot, the demo's tab UI behavior), title lagging on the next push.
expect 'demo: browsers-changed n=2' "$log" create: push
expect 'demo: browser-row id=2 url=.*history-second-page.html .*windowed=0' "$log" create: row

# Title change: the background tab's title lands without a select.
expect 'demo: browser-row id=2 url=.*browser-data-page.html title=Browser data active=0' "$log" address: background row updated

# Select: the row's active flag moved.
expect 'demo: browser-row id=1 .*active=1' "$log" select: survivor active
expect 'demo: browser-row id=2 .*active=0' "$log" select: background inactive

# Close: the row is gone and the table says so.
LAST=$(grep -c 'demo: browsers-changed' "$log")
if [ "$LAST" -ge 4 ]; then ok "close: table pushed again ($LAST times)"
else bad "close: table pushed only $LAST times"; fi
if grep -qE 'demo: browsers-changed n=1' "$log"; then
  ok "close: back to one row"
else
  bad "close: back to one row"
fi

# The read-back step (browsers) prints the cache; its rows are the last ones
# in the log, and the closed browser must not be among them.
expect 'demo: browsers-changed n=1' "$log" read: one row
LASTROW=$(grep 'browser-row id=' "$log" | tail -1)
if echo "$LASTROW" | grep -q 'id=1 ' && ! echo "$LASTROW" | grep -q 'id=2 '; then
  ok "read: final cache names only the survivor"
else
  bad "read: final cache is '$LASTROW'"
fi

echo
if [ $fails = 0 ]; then printf 'browsers gate: all checks passed\n'
else printf 'browsers gate: %d check(s) failed\n' "$fails"; exit 1; fi
