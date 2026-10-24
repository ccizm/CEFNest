#!/usr/bin/env bash
#
# test/occlusion-gate.sh — a fully covered window throttles its page.
#
# Mutter stops sending frame callbacks to a window nothing of is visible
# (measured on this desktop: a covered victim's frame clock went from 116
# ticks/2s to 12/4s and sprang back to 144/2s on reveal). Nothing told the
# widget about that, so its watchdog handed pacing to a 16ms timer and a
# covered page kept committing at full refresh — 56 commits/s for a window
# nobody could see. The occlusion heartbeat reads the quiet instead: a
# single-window session whose clock answered once and then went silent is
# paced at one frame's worth of answers per 500ms, and any after-paint —
# a revealed window paints — hands pacing straight back to the clock.
#
# This gate covers the demo's window with a fullscreen opaque window and
# asserts all three phases by the compositor's commit counter:
#   visible:  full rate          (>= 25 commits/s)
#   covered:  heartbeat rate     (<= 10 commits/s)
#   revealed: full rate again    (>= 20 commits within 2s of the reveal)
#
# The occlusion signal only exists on a real display — a headless nested
# session never answers the clock at all, where the latch keeps the feature
# inert and the plain timer paces the page (that is the headless safety the
# whole suite already asserts). So under ci-session.sh this gate skips.
#
#   ./test/occlusion-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
COVER="$ROOT/builddir/test/cover-window"
. "$ROOT/test/helper-path.sh"

if [ -n "${CEFNEST_HEADLESS:-}" ]; then
  echo "occlusion gate: SKIP (no occlusion signal on a headless display)"
  exit 77
fi
if [ ! -x "$DEMO" ]; then
  echo "occlusion gate: SKIP (no demo binary; build with meson)"
  exit 77
fi
if [ ! -x "$COVER" ]; then
  echo "occlusion gate: SKIP (no cover-window binary)"
  exit 77
fi

PAGE=$(python3 -c "import urllib.parse; print('data:text/html,'+urllib.parse.quote(\"<html><body style='margin:0'><script>let t=0;function f(){t++;document.body.style.background='hsl('+(t%360)+',80%,50%)';requestAnimationFrame(f)}f()</script></body></html>\"))")

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; [ -n "${demo_pid:-}" ] && kill "$demo_pid" 2>/dev/null; true' EXIT
LOG="$WORK/demo.log"

fails=0
ok()   { printf '  ok   %s\n' "$1"; }
fail() { printf '  FAIL %s\n' "$1"; fails=$((fails+1)); }

# The last per-second commit counter the demo printed.
counter() {
  grep -o 'dmabuf=[0-9]*' "$LOG" | tail -1 | tr -d 'a-z='
}

CHROMIUM_FRAME_METRICS=1 CHROMIUM_URL="$PAGE" "$DEMO" >"$LOG" 2>&1 &
demo_pid=$!

# Let the page come up and settle at full rate.
sleep 4
c0=$(counter)
sleep 2
c1=$(counter)
visible_rate=$(( (c1 - c0) / 2 ))

"$COVER" 6 &
# Discard the ramp: the watchdog needs its 100ms and the last counter line
# can be up to a second stale, both of which are still visible-window time.
sleep 2
c2=$(counter)
sleep 3
c3=$(counter)
covered_rate=$(( (c3 - c2) / 3 ))

# The cover closes on its own; the reveal must hand pacing back at once.
wait %2 2>/dev/null
c4=$(counter)
sleep 2
c5=$(counter)

kill "$demo_pid" 2>/dev/null
wait "$demo_pid" 2>/dev/null
demo_pid=

echo "     commits: visible ${visible_rate}/s, covered ${covered_rate}/s, revealed $(( (c5 - c4) )) in 2s (from counter $c5)"

if [ "$visible_rate" -ge 25 ]; then
  ok "page runs at full rate while visible ($visible_rate/s)"
else
  fail "page ran at $visible_rate/s while visible (wanted >= 25)"
fi
if [ "$covered_rate" -le 10 ]; then
  ok "covered page throttled to heartbeat ($covered_rate/s, wanted <= 10)"
else
  fail "covered page kept committing at $covered_rate/s (wanted <= 10)"
fi
if [ $((c5 - c4)) -ge 20 ]; then
  ok "revealed page recovers to full rate ($((c5 - c4)) commits in 2s)"
else
  fail "revealed page made only $((c5 - c4)) commits in 2s (wanted >= 20)"
fi

if [ "$fails" -eq 0 ]; then
  echo "occlusion gate: all checks passed"
  exit 0
fi
echo "occlusion gate: $fails check(s) failed"
exit 1
