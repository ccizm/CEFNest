#!/usr/bin/env bash
#
# test/frame-gate.sh — the frame tree reaches the host, event by event.
#
# The probe page holds one static same-origin iframe and adds/removes a
# second one from script. The gate asserts: the main frame and the static
# child announce themselves with the right is_main flags (created-time URLs
# are empty — CEF only knows the URL once the navigation commits); the
# count probes bracket the dynamic iframe at 1 → 2 → 1; exactly one frame
# is destroyed during the run (the removed one) and its identifier matches
# a created event; the attach phase produced frame-attached traffic.
#
#   ./test/frame-gate.sh
#   ./test/ci-session.sh ./test/frame-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/frame-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'frame|demo: script' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat >"$WORK/frame.session" <<EOF
load $PAGE
wait 2500
js __frames()
wait 500
js document.getElementById("add").click()
wait 1500
js __frames()
wait 500
js document.getElementById("remove").click()
wait 1500
js __frames()
wait 500
quit
EOF

echo "== frame lifecycle"
timeout --kill-after=10s 120 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/frame.session" \
  "$DEMO" >"$WORK/frame.log" 2>&1
status=$?
log=$WORK/frame.log
if [ "$status" = 0 ]; then ok "frame: demo exited 0"
else bad "frame: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: frame-created browser=1 frame=[0-9A-Za-z-]+ main=1 url=' "$log" main-created
expect 'demo: frame-created browser=1 frame=[0-9A-Za-z-]+ main=0 url=' "$log" child-created
expect 'demo: main-frame-changed browser=1 old= new=[0-9A-Za-z-]+' "$log" main-frame-changed

# The five probes read, in order: count 1, click (null), count 2,
# click (null), count 1.
values=$(sed -n 's/^demo: script [0-9]* ok value=\(.*\)$/\1/p' "$log" | tr '\n' ' ')
if [ "$values" = "1 null 2 null 1 " ]; then ok "counts: 1 → 2 → 1"
else bad "counts: wanted '1 null 2 null 1 ', got '$values'"; fi

# Exactly one frame dies during the run — the removed one — and its id must
# have been announced by a created event first.
destroyed=$(grep -cE 'demo: frame-destroyed browser=1 frame=' "$log")
if [ "$destroyed" = "1" ]; then ok "destroyed: exactly one"
else bad "destroyed: wanted exactly 1, got $destroyed"; fi

DYN=$(sed -n 's/^demo: frame-destroyed browser=1 frame=\(.*\)$/\1/p' "$log" | tail -1)
if [ -n "$DYN" ] && grep -qE "demo: frame-created browser=1 frame=$DYN " "$log"; then
  ok "dynamic: created/destroyed pair for frame=$DYN"
else
  bad "dynamic: no created event matching destroyed frame=$DYN"
fi

expect 'demo: frame-attached browser=1 frame=[0-9A-Za-z-]+' "$log" attached

echo
if [ $fails = 0 ]; then printf 'frame gate: all checks passed\n'
else printf 'frame gate: %d check(s) failed\n' "$fails"; exit 1; fi
