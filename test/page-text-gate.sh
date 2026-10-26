#!/usr/bin/env bash
#
# test/page-text-gate.sh — CefFrame::GetText, streamed and reassembled.
#
# The page carries a marker in its own DOM; the helper streams the frame's text
# as page-text-begin / chunk* / end. The gate grades the two things a host
# depends on: every chunk arrived (the end's reassembled byte count equals the
# begin's promised count) and the content is the page's text, not an empty
# string or an error document.
#
#   ./test/page-text-gate.sh
#   ./test/ci-session.sh ./test/page-text-gate.sh

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
    grep -nE 'page-text|loaded|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/text.session" <<EOF
load $PAGE
wait 2000
page-text
wait 2500
close
EOF

echo "== page text"
timeout --kill-after=10s 60 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/text.session" \
  "$DEMO" >"$WORK/text.log" 2>&1
status=$?
log=$WORK/text.log
if [ "$status" = 0 ]; then ok "text: demo exited 0"
else bad "text: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: loaded file://.*browser-data-page\.html \(HTTP 200\)' "$log" page
expect 'demo: page-text-begin id=[0-9]+ browser=1 bytes=[1-9][0-9]* truncated=0' \
  "$log" begin
expect 'demo: page-text id=[0-9]+ bytes=[1-9][0-9]* truncated=0 text=".*CEFNEST-PAGE-TEXT-MARKER.*Second line, also in the text extraction\."' \
  "$log" content

promised=$(sed -n 's/^demo: page-text-begin id=[0-9]* browser=[0-9]* bytes=\([0-9]*\) .*/\1/p' "$log" | head -1)
delivered=$(sed -n 's/^demo: page-text id=[0-9]* bytes=\([0-9]*\) .*/\1/p' "$log" | head -1)
if [ -n "$promised" ] && [ "$promised" = "$delivered" ]; then
  ok "text: $delivered bytes arrived of $promised promised"
else
  bad "text: begin promised '$promised', reassembly got '$delivered'"
fi

echo
if [ $fails = 0 ]; then printf 'page-text gate: all checks passed\n'
else printf 'page-text gate: %d check(s) failed\n' "$fails"; exit 1; fi
