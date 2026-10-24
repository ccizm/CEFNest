#!/usr/bin/env bash
#
# test/frame-stop-gate.sh — a hidden window stops costing frames.
#
# Nothing tells the helper's Chromium about host window visibility, so without
# help a hidden window's page kept producing commits at full refresh rate —
# measured 2026-10-21 (.memory/measured-facts.md §9, rAF 61.5/s) and again at
# the commit level 2026-10-24: an animating page hidden for 2.4s kept ~144
# commits flowing into shm copies / GPU work for a window nobody can see. The
# fix suspends the hidden view's toplevel: frame callbacks queue unanswered,
# and the commit pipeline — which paces on wl_surface.frame — stalls with them.
#
# The page's rAF counter is NOT the assertion: the renderer self-paces its
# BeginFrames and keeps ticking while hidden (measured 2026-10-24 on both a
# real desktop and a headless nested session). What stops is the commit
# pipeline, and that is what `frames` — the compositor's commit counter —
# asserts here: flowing while visible, frozen while hidden, flowing again
# after the window comes back.
#
#   ./test/frame-stop-gate.sh
#   ./test/ci-session.sh ./test/frame-stop-gate.sh
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

# A page that damages on every rAF, so every animation frame is a commit.
cat >"$WORK/anim.html" <<'HTML'
<!doctype html><html><body style="margin:0">
<canvas id=c width=600 height=400></canvas>
<script>
const ctx=c.getContext('2d');let t=0;
function draw(){t++;ctx.fillStyle=`hsl(${t%360},80%,50%)`;ctx.fillRect(0,0,600,400);requestAnimationFrame(draw)}
draw();
</script></body></html>
HTML

# Session step lines (1-based, as demo numbers its script results):
#   3 rAF installer   5,7 frames visible   10,12 frames hidden
#  15 frames resumed  16 rAF read back
cat >"$WORK/frame-stop.session" <<SESSION
load file://$WORK/anim.html
wait 2500
js-expr (()=>{window.__raf=0;const f=()=>{window.__raf++;requestAnimationFrame(f)};requestAnimationFrame(f);return "raf"})()
wait 1200
frames
wait 1200
frames
hide
wait 1200
frames
wait 1200
frames
show
wait 1500
frames
js-expr window.__raf
wait 500
quit
SESSION

echo "== hidden window stops committing"
timeout 120 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/frame-stop.session" \
  "$DEMO" >"$WORK/out" 2>"$WORK/err"
rc=$?
cat "$WORK/err" >>"$WORK/out"
if [ $rc != 0 ]; then bad "demo exited $rc"; fi

mapfile -t commits < <(grep -ao "demo: frames [0-9]*" "$WORK/out" | awk '{print $3}')
if [ ${#commits[@]} != 5 ]; then
  bad "expected 5 frames reads, got ${#commits[@]}"
  tail -15 "$WORK/out" | sed 's/^/         /'
else
  fA=${commits[0]}; fB=${commits[1]}; fC=${commits[2]}; fD=${commits[3]}; fE=${commits[4]}
  visible=$((fB - fA)); hidden1=$((fC - fB)); hidden2=$((fD - fC)); resumed=$((fE - fD))
  echo "     commits: visible $visible, hidden $hidden1/$hidden2, resumed $resumed"
  if [ $visible -ge 40 ]; then ok "page animates while visible ($visible commits/1.2s)"
  else bad "page barely animates while visible ($visible commits/1.2s)"; fi
  if [ $hidden1 -le 2 ]; then ok "hidden window freezes commits ($hidden1/1.2s)"
  else bad "hidden window kept committing ($hidden1/1.2s)"; fi
  if [ $hidden2 -le 2 ]; then ok "hidden window stays frozen ($hidden2/1.2s)"
  else bad "hidden window resumed committing ($hidden2/1.2s)"; fi
  if [ $resumed -ge 40 ]; then ok "page animates again after show ($resumed commits/1.2s)"
  else bad "page did not resume after show ($resumed commits/1.2s)"; fi
fi

raf=$(grep -ao 'script 16 ok value=[0-9]*' "$WORK/out" | grep -ao '[0-9]*$')
if [ -n "$raf" ] && [ "$raf" -ge 30 ]; then
  ok "renderer still runs its rAF loop after reshow ($raf ticks)"
else
  bad "rAF did not read back after reshow (got '$raf')"
fi

echo
if [ $fails = 0 ]; then printf 'frame-stop gate: all checks passed\n'
else printf 'frame-stop gate: %d check(s) failed\n' "$fails"; exit 1; fi
