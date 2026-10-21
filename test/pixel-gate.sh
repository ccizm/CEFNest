#!/usr/bin/env bash
#
# test/pixel-gate.sh — asserts that what the page painted reaches the widget.
#
# Every other gate reads the browser through logs: a scroll position, a find
# count, a menu model. Those say the page is alive; none of them say a pixel of
# it ever arrived. This one does. test/pixel-page.html fills the viewport with
# four flat colours, the demo dumps the view (CHROMIUM_SESSION_SCRIPT's `dump`
# step, which grabs both the composited widget and the client texture it was
# built from), and the gate samples the middle of each quadrant and compares
# against the rgb the page asked for. Exact, not approximate: measured inside
# test/ci-session.sh the buffer comes back 18,52,86 / 200,50,60 / 43,138,62 /
# 232,232,232 — the same bytes the CSS says.
#
# What it would have caught: the whole-page freeze of docs/frame-clock-pacing.md
# §8. That bug passed every log-based gate — the page scrolled, find answered, the
# menu built — while the buffer held nothing but the colour Chromium had cleared it
# to. A flat buffer is exactly what this gate refuses to accept.
#
#   ./test/ci-session.sh ./test/pixel-gate.sh   # the run this gate is for
#   ./test/pixel-gate.sh                        # on a real display, same assertions
#
# It asserts on both displays, and the first version of this file did not: it skipped
# the pixel checks whenever a host run came back flat, on the reading that a window
# nobody is looking at paints nothing. That reading was wrong and the skip was close
# to costly. The flat #101828 was a regression in the component — bcbebc8 moved the
# frame callbacks off the clock path, so any display whose clock answered never got a
# client painted again, on this laptop and in CI alike. Measured the same day: the
# page's own requestAnimationFrame counter stopped at 4 callbacks across 4.5 s, then
# reached 252 with the callbacks fired from after-paint again, and the dump went from
# #101828 to the page's own green. docs/headless-ci.md §5 keeps the numbers.
#
# The lesson is the reason there is no skip left here: a flat buffer is a statement
# about the component, never about who is looking at it. If a future display really
# does pause an off-screen window, this gate fails on that machine and says so with
# the page's own readback in the log above the failure.
#
# Needs ImageMagick (identify/convert); without it the gate exits 77 and says it
# asserted nothing, which is not the same verdict as passing.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/pixel-page.html"

fails=0
skips=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
skip() { skips=$((skips + 1)); printf '  skip %s\n' "$1"; }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'dump:|loaded|script' "$2" | head -6 | sed 's/^/         /'
  fi
}

n_colors() { identify -format '%k' "$1" 2>/dev/null; }

sample() { # sample <png> <frac-x> <frac-y> — mean of a 9x9 patch as r,g,b
  local png=$1 fx=$2 fy=$3 w h x y
  w=$(identify -format '%w' "$png"); h=$(identify -format '%h' "$png")
  x=$(awk -v w="$w" -v f="$fx" 'BEGIN{printf "%d", w*f}')
  y=$(awk -v h="$h" -v f="$fy" 'BEGIN{printf "%d", h*f}')
  convert "$png" -crop "9x9+$((x - 4))+$((y - 4))" +repage -depth 8 \
    -format '%[fx:int(255*r+0.5)],%[fx:int(255*g+0.5)],%[fx:int(255*b+0.5)]' info:
}

# <frac-x> <frac-y> <r,g,b> <name>, the same order the page declares them in.
QUADS=("0.25 0.25 18,52,86 top-left"
       "0.75 0.25 200,50,60 top-right"
       "0.25 0.75 43,138,62 bottom-left"
       "0.75 0.75 232,232,232 bottom-right")

expect_quads() { # expect_quads <png> <tag>
  local png=$1 tag=$2 q x y want name got any=0
  for q in "${QUADS[@]}"; do
    set -- $q; x=$1 y=$2 want=$3 name=$4
    got=$(sample "$png" "$x" "$y")
    if [ "$got" = "$want" ]; then ok "$tag/$name: $got"
    else bad "$tag/$name: buffer says $got, the page asked for $want"; any=1; fi
  done
  return $any
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if ! command -v identify >/dev/null || ! command -v convert >/dev/null; then
  # 77, not 0: meson calls a test that could not run SKIPPED with exactly that
  # status, and ci-all.sh shows it as SKIP. Every pixel check needs both tools, so
  # a machine without them asserts nothing here — and a gate that asserts nothing
  # has no business exiting like a pass.
  echo "no identify(1)/convert(1) — install imagemagick to run this gate"
  printf '  skip every pixel check\n'
  printf 'pixel gate: skipped, it could not assert anything\n'
  exit 77
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

printf 'load %s\nwait 1500\njs Array.from(document.querySelectorAll(".q")).map(e=>getComputedStyle(e).backgroundColor).join(",")\nwait 2000\ndump %s\n' \
  "$PAGE" "$WORK/pixel.png" >"$WORK/pixel.session"

echo "== one run: a page of four flat fills, dumped once"
timeout 90 env GDK_BACKEND=wayland CHROMIUM_SESSION_SCRIPT="$WORK/pixel.session" \
  "$DEMO" >"$WORK/pixel.out" 2>"$WORK/pixel.err"
status=$?
cat "$WORK/pixel.err" >>"$WORK/pixel.out"

if [ $status -ne 0 ]; then
  bad "the demo exited with $status"
  tail -8 "$WORK/pixel.out" | sed 's/^/         /'
elif [ ! -f "$WORK/pixel.png" ]; then
  bad "no dump written at $WORK/pixel.png"
  tail -8 "$WORK/pixel.out" | sed 's/^/         /'
else
  expect 'loaded file://.*pixel-page\.html' "$WORK/pixel.out" page
  expect 'client buffer [1-9][0-9]*x[1-9][0-9]*' "$WORK/pixel.out" geometry
  expect 'value="rgb\(18, 52, 86\),rgb\(200, 50, 60\),rgb\(43, 138, 62\),rgb\(232, 232, 232\)"' \
    "$WORK/pixel.out" "the page itself"

  client=$WORK/pixel.png.client.png
  if [ -f "$client" ]; then
    expect_quads "$client" client-texture
  else
    skip "no $client — this build dumps only the composited widget"
  fi
  expect_quads "$WORK/pixel.png" widget
  if [ "$(n_colors "$WORK/pixel.png")" -gt 3 ]; then
    ok "widget: $(n_colors "$WORK/pixel.png") distinct colours, not a flat buffer"
  else
    bad "widget: $(n_colors "$WORK/pixel.png") distinct colours — the four fills did not arrive"
  fi
fi

echo
if [ $fails = 0 ] && [ $skips = 0 ]; then printf 'pixel gate: all checks passed\n'
elif [ $fails = 0 ]; then printf 'pixel gate: all checks passed, %d skipped\n' "$skips"
else printf 'pixel gate: %d check(s) failed, %d skipped\n' "$fails" "$skips"; exit 1; fi
