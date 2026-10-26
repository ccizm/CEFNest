#!/usr/bin/env bash
#
# test/ua-gate.sh — the user agent as a live property.
#
# The page reads navigator.userAgent three times: as Chromium built it, under
# a session-wide override, and after the override is cleared. The re-applied
# override survives a navigation (it rides every load start), which is what
# makes a UA switcher a property instead of a restart.
#
#   ./test/ua-gate.sh
#   ./test/ci-session.sh ./test/ua-gate.sh

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
    grep -nE 'demo: script|ua-override|ua-set' "$2" | tail -8 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/ua.session" <<EOF
load $PAGE
wait 1500
js navigator.userAgent
wait 600
ua-set Mozilla/5.0 CEFNestProbe/7
wait 800
load $PAGE
wait 1500
js navigator.userAgent
wait 600
ua-clear
wait 800
load $PAGE
wait 1500
js navigator.userAgent
wait 600
quit
EOF

echo "== navigator.userAgent: default, override, cleared"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/ua.session" \
  "$DEMO" >"$WORK/ua.log" 2>&1
status=$?
log=$WORK/ua.log
if [ "$status" = 0 ]; then ok "ua: demo exited 0"
else bad "ua: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

VALUES=$(grep -oE 'demo: script [0-9]+ ok value=.*' "$log" | sed 's/.*value=//')
COUNT=$(printf '%s\n' "$VALUES" | grep -c . )
if [ "$COUNT" = 3 ]; then ok "ua: three reads"
else bad "ua: wanted 3 reads, got $COUNT"; fi

echo "$VALUES" | head -1 | grep -q 'CEFNestProbe' \
  && bad "ua: default read already carries the override" \
  || ok "ua: default read is Chromium's own"
echo "$VALUES" | sed -n 2p | grep -q 'CEFNestProbe/7' \
  && ok "ua: override read carries CEFNestProbe/7" \
  || bad "ua: override read is '$(echo "$VALUES" | sed -n 2p)'"
echo "$VALUES" | sed -n 3p | grep -q 'CEFNestProbe' \
  && bad "ua: cleared read still carries the override" \
  || ok "ua: cleared read is back to Chromium's own"

echo
if [ $fails = 0 ]; then printf 'ua gate: all checks passed\n'
else printf 'ua gate: %d check(s) failed\n' "$fails"; exit 1; fi
