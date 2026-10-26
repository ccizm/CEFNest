#!/usr/bin/env bash
#
# test/page-events-gate.sh — the two page-state events Batch 1 added.
#
# One scripted run against the real CEF helper and the demo:
#
#   loading-progress  progress arrives while the page loads, before the load
#                     answer, and completes at 1.0
#   fullscreen        a click on the page's button enters fullscreen and
#                     document.exitFullscreen() leaves it; both directions
#                     reach ::fullscreen-changed and the page's own state
#
# The fullscreen request needs transient user activation, so the click is
# injected into the demo's nested compositor — never at the desktop pointer.
#
#   ./test/page-events-gate.sh
#   ./test/ci-session.sh ./test/page-events-gate.sh
#   KEEP_LOGS=1 ./test/page-events-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/page-events-page.html"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then
    echo "  note: transcripts kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'loading|fullscreen|script [0-9]+' "$2" | head -12 | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_after() { # expect_after <earlier> <later> <logfile> <tag>
  local a b
  a=$(grep -nE "$1" "$3" | head -1 | cut -d: -f1)
  b=$(grep -nE "$2" "$3" | head -1 | cut -d: -f1)
  if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ]; then
    ok "$4: /$2/ (line $b) came after /$1/ (line $a)"
  else
    bad "$4: wanted /$2/ after /$1/ — got '${b:-none}' and '${a:-none}'"
  fi
}

js_value() { # js_value <logfile> <n>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat > "$WORK/events.session" <<EOF
load $PAGE
wait 2500
js document.getElementById("out").textContent
wait 500
click 160 160
wait 1500
js document.getElementById("out").textContent
wait 500
js document.exitFullscreen()
wait 1000
js document.getElementById("out").textContent
wait 800
close
EOF

echo "== events"
timeout --kill-after=10s 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/events.session" \
  "$DEMO" 2>&1 | tee "$WORK/events.log" | cat > /dev/null
status=${PIPESTATUS[0]}
log=$WORK/events.log
if [ "$status" = 0 ]; then ok "events: demo exited 0"
else bad "events: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# The first progress report proves the event exists at all; the ordering proves
# it is progress and not a second notification after the fact.
expect 'helper: loading progress 0\.[0-9]+' "$log" events
expect 'demo: loading-progress 0\.[0-9]+' "$log" events
expect_after 'demo: loading-progress 0\.[0-9]+' \
  'demo: loaded .*page-events-page\.html \(HTTP 200\)' "$log" events
expect 'demo: loading-progress 1\.000' "$log" events
[ "$(js_value "$log" 1)" = "PAGE-EVENTS-START" ] \
  && ok "events: the page started without fullscreen" \
  || bad "events: first read-back was '$(js_value "$log" 1)'"

expect 'helper: page fullscreen mode = 1' "$log" events
expect 'demo: fullscreen 1' "$log" events
case "$(js_value "$log" 2)" in
  *request=ok*fullscreen=on*) ok "events: a real click entered fullscreen" ;;
  *) bad "events: second read-back was '$(js_value "$log" 2)'" ;;
esac
expect_after 'demo: fullscreen 1' \
  'demo: script [0-9]+ ok value="PAGE-EVENTS-START\|request=ok\|fullscreen=on"' \
  "$log" events

expect 'helper: page fullscreen mode = 0' "$log" events
expect 'demo: fullscreen 0' "$log" events
case "$(js_value "$log" 3)" in
  *fullscreen=off*) ok "events: exitFullscreen left fullscreen" ;;
  *) bad "events: third read-back was '$(js_value "$log" 3)'" ;;
esac
expect_no 'fullscreen=error' "$log" events

echo
if [ "$fails" -eq 0 ]; then
  printf 'page events gate: passed\n'
else
  printf 'page events gate: %d check(s) failed\n' "$fails"
  tail -60 "$log" | sed 's/^/  /'
  exit 1
fi
