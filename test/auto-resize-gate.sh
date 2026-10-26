#!/usr/bin/env bash
#
# test/auto-resize-gate.sh — asserts the auto-resize property end to end.
#
# The page's box is resized three times by script:
#
#   420x300   inside the 300x200..500x400 window, reported unchanged
#   650x500   over the maximum, clamped to 500x400
#   100x100   under the minimum, clamped to 300x200
#
# The helper's OnAutoResize is the producer; the component turns it into a
# nested-compositor configure, and the surface-size step reads the buffer the
# CEF window committed afterwards. Both halves are asserted, because a size the
# host only logged and never applied would leave the page in a 1000x700 window.
#
#   ./test/auto-resize-gate.sh
#   ./test/ci-session.sh ./test/auto-resize-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/auto-resize-page.html"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then echo "  note: transcripts kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'auto-resize|surface-size|loaded' "$2" | head -12 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat > "$WORK/resize.session" <<'EOF'
load PAGE
wait 2500
surface-size 1
js setSize(650,500)
wait 1500
surface-size 1
js setSize(100,100)
wait 1500
surface-size 1
auto-resize off
wait 300
js setSize(700,600)
wait 1000
surface-size 1
close
EOF
sed -i "s|PAGE|$PAGE|" "$WORK/resize.session"

echo "== auto-resize"
timeout --kill-after=10s 120 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CEFNEST_AUTO_RESIZE=1 \
  CEFNEST_AUTO_RESIZE_BOUNDS="300 200 500 400" \
  CHROMIUM_SESSION_SCRIPT="$WORK/resize.session" \
  "$DEMO" >"$WORK/resize.log" 2>&1
status=$?
log=$WORK/resize.log
if [ "$status" = 0 ]; then ok "auto-resize: demo exited 0"
else bad "auto-resize: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'helper: auto-resize on min=300x200 max=500x400 \(browser 1\)' "$log" auto-resize
expect 'helper: auto-resize 420x300 \(browser 1\)' "$log" auto-resize
expect 'demo: auto-resize 420x300 browser=1' "$log" auto-resize
expect 'helper: auto-resize 500x400 \(browser 1\)' "$log" auto-resize
expect 'helper: auto-resize 300x200 \(browser 1\)' "$log" auto-resize

# The surface sizes are read after each page change settled, in the same order:
# unchanged, clamped high, clamped low. The last read is after the property was
# switched off, so the 700x600 box changes nothing — the CEF window keeps the
# size it had, and there is no 700x600 notification anywhere.
got=$(sed -n 's/^demo: surface-size browser=1 size=\(.*\)$/\1/p' "$log" | tr '\n' ' ')
case "$got" in
  "420x300 500x400 300x200 300x200 "*)
    ok "auto-resize: surfaces were $got" ;;
  *)
    bad "auto-resize: surfaces were '$got', wanted 420x300 500x400 300x200 300x200" ;;
esac
if grep -q 'helper: auto-resize 700x600' "$log"; then
  bad "auto-resize: a disabled property still reported 700x600"
else ok "auto-resize: off stopped the notifications"; fi

echo
if [ $fails = 0 ]; then printf 'auto-resize gate: all checks passed\n'
else printf 'auto-resize gate: %d check(s) failed\n' "$fails"; exit 1; fi
