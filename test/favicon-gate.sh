#!/usr/bin/env bash
#
# test/favicon-gate.sh — the page's declared icon URLs reach the host.
#
# The helper stops at the URL list: it downloads nothing and renders nothing.
# This gate loads a page with an icon link over file:// and asserts both sides
# of the relay — the helper's "favicon N url(s)" log and the demo's
# ::favicon-changed line naming the icon.
#
#   ./test/favicon-gate.sh
#   ./test/ci-session.sh ./test/favicon-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/browser-data-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'favicon|loaded|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if [ ! -f "$ROOT/test/favicon.png" ]; then echo "no test/favicon.png"; exit 1; fi

cat >"$WORK/favicon.session" <<EOF
load $PAGE
wait 2500
close
EOF

echo "== favicon"
timeout --kill-after=10s 60 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/favicon.session" \
  "$DEMO" >"$WORK/favicon.log" 2>&1
status=$?
log=$WORK/favicon.log
if [ "$status" = 0 ]; then ok "favicon: demo exited 0"
else bad "favicon: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: loaded file://.*browser-data-page\.html \(HTTP 200\)' "$log" page
expect 'helper: favicon 1 url\(s\)' "$log" helper
expect 'demo: favicon browser=1 urls=file://.*/test/favicon\.png' "$log" signal

echo
if [ $fails = 0 ]; then printf 'favicon gate: all checks passed\n'
else printf 'favicon gate: %d check(s) failed\n' "$fails"; exit 1; fi
