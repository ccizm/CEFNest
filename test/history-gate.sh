#!/usr/bin/env bash
#
# test/history-gate.sh — CefBrowserHost::GetNavigationEntries through the demo.
#
# Two pages, then one history request. The second page's entry must be the
# current one and both titles must be there, which is the difference between
# "the command answered" and "it answered the browser's real history".
#
#   ./test/history-gate.sh
#   ./test/ci-session.sh ./test/history-gate.sh

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
    grep -nE 'history|loaded|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/history.session" <<EOF
load $FIRST
wait 1800
load $SECOND
wait 1800
history
wait 2500
close
EOF

echo "== history"
timeout --kill-after=10s 60 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/history.session" \
  "$DEMO" >"$WORK/history.log" 2>&1
status=$?
log=$WORK/history.log
if [ "$status" = 0 ]; then ok "history: demo exited 0"
else bad "history: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: history id=[0-9]+ json=.*"current_index":1' "$log" current
expect 'demo: history id=[0-9]+ json=.*"index":0,"title":"Browser data"' "$log" first entry
expect 'demo: history id=[0-9]+ json=.*"index":1,"title":"History second page"' "$log" second entry
expect 'demo: history id=[0-9]+ json=.*"current":true' "$log" current flag

if [ "$(grep -c 'demo: history id=' "$log")" = 1 ]; then
  ok "history: one request, one answer"
else
  bad "history: $(grep -c 'demo: history id=' "$log") answers for one request"
fi

echo
if [ $fails = 0 ]; then printf 'history gate: all checks passed\n'
else printf 'history gate: %d check(s) failed\n' "$fails"; exit 1; fi
