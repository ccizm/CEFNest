#!/usr/bin/env bash
#
# test/surface-pair-gate.sh — the threshold the plugin plan set for "搭法二":
# is browser id N's paired surface really the surface that draws browser N?
#
# Four scenarios, each one a scripted run against the real CEF helper, each
# page a unique solid colour so the answer is a pixel:
#
#   1. same-open   a plugin's popup and options pages open together
#   2. reuse       one window closes, another opens — the address-reuse worry
#   3. devtools    the inspector window is open too, then another window opens
#   4. four        four plugins' popup pages open at once
#
# The pairing under test is the session's own (::browser-surface and
# chromium_session_get_browser_surface()), not the demo's FIFO window map:
# script_surface_probe() reads the pixel from the surface the session paired
# with the id. If the session pairs id 3 with id 2's surface, the sample for
# id 3 comes back red instead of green and the scenario fails.
#
# The decision this gate feeds (work-plan-2026-10-26 §5.2): all four scenarios
# pass -> the interface lands; any one fails -> no interface, and 搭法二 is
# "the host's own risk", with the measurement recorded. The threshold run is a
# real-desktop run. Under ci-session.sh (headless mutter) the gate re-executes
# itself under Xvfb the way devtools-gate.sh does, because scenario 3 needs
# gtk_popover_popup() to return.
#
# Nothing here touches the pointer or keyboard of the desktop: the one click
# goes into the demo's own nested compositor.
#
#   ./test/surface-pair-gate.sh
#   ./test/ci-session.sh ./test/surface-pair-gate.sh
#   KEEP_LOGS=1 ./test/surface-pair-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

if [ "${CEFNEST_HEADLESS:-0}" = 1 ] && [ -z "${SURFACE_PAIR_GATE_XVFB:-}" ]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    export SURFACE_PAIR_GATE_XVFB=1
    exec xvfb-run -a env -u WAYLAND_DISPLAY GDK_BACKEND=x11 "$0" "$@"
  fi
  printf '  skip surface-pair: no xvfb-run, and headless mutter does not return from gtk_popover_popup()\n'
  printf 'surface-pair gate: skipped, it could not assert anything\n'
  exit 77
fi

[ -x "$DEMO" ] || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
[ -x "$HELPER" ] || { echo "no helper at $HELPER"; exit 1; }

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

# The helper, Chromium's cache and the demo's nested-compositor socket all land
# in XDG_RUNTIME_DIR; every run gets one nobody else reads (made in run_demo).

# Four plugins, each one page in one colour. The extension scheme serves them
# from the plugin roots, so what the surface samples is what the plugin's own
# directory holds.
plugin_page() { # plugin_page <plugin-dir> <file> <title> <hex>
  cat > "$WORK/plugins/$1/$2" <<EOF
<!doctype html>
<meta charset="utf-8">
<title>$3</title>
<style>html, body { margin: 0; height: 100%; background: #$4; }</style>
EOF
}

plugin_manifest() { # plugin_manifest <id> <name>
  mkdir -p "$WORK/plugins/$1"
  printf '{"manifest_version": 3, "name": "%s", "version": "1.0", "cefnest": {"type": "web"}}\n' \
    "$2" > "$WORK/plugins/$1/cefnest-plugin.json"
}

plugin_manifest pair-a "Pair A"
plugin_page pair-a popup.html "pair-a popup" e01b24     # red
plugin_page pair-a options.html "pair-a options" 1c71d8 # blue
plugin_manifest pair-b "Pair B"
plugin_page pair-b popup.html "pair-b popup" 2ec27e     # green
plugin_page pair-b options.html "pair-b options" ff7800
plugin_manifest pair-c "Pair C"
plugin_page pair-c popup.html "pair-c popup" f5c211     # yellow
plugin_manifest pair-d "Pair D"
plugin_page pair-d popup.html "pair-d popup" c061cb     # magenta

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    tail -20 "$2" | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

RC=0
run_demo() { # run_demo <tag> <script> [demo args...]
  local tag=$1 script=$2
  shift 2
  echo "== $tag"
  # A private runtime dir per run: the demo's nested compositor leaves its
  # lock file behind, and the next run in the same dir would warn about it.
  mkdir -p "$WORK/xdg-$tag"
  chmod 700 "$WORK/xdg-$tag"
  XDG_RUNTIME_DIR="$WORK/xdg-$tag" \
    CHROMIUM_SESSION_SCRIPT=$WORK/$script CHROMIUM_HELPER=$HELPER \
    timeout --kill-after=10s 120 "$DEMO" "$@" </dev/null >"$WORK/$tag.log" 2>&1
  RC=$?
  if [ "$RC" = 0 ]; then ok "$tag: demo exited 0"
  else bad "$tag: demo exited $RC"; tail -20 "$WORK/$tag.log" | sed 's/^/         /'; fi
}

write_script() { # write_script <file> <body>
  printf '%s\n' "$2" > "$WORK/$1"
}

# The last sampled pixel for a browser id: "r g b" on stdout, or nothing.
sample_rgb() { # sample_rgb <logfile> <id>
  local line
  line=$(grep -E "^demo: surface-sample browser=$2 " "$1" | tail -1)
  case "$line" in
    *rgba=none*) return 0 ;;
  esac
  printf '%s\n' "$line" |
    sed -n 's/.*rgba=\([0-9]*\),\([0-9]*\),\([0-9]*\),[0-9]*$/\1 \2 \3/p'
}

near() { # near <got> <want> — within 16, which covers an sRGB round trip
  local d=$(( $1 - $2 ))
  [ "$d" -ge -16 ] && [ "$d" -le 16 ]
}

expect_color() { # expect_color <logfile> <id> <r> <g> <b> <tag>
  local got r g b
  got=$(sample_rgb "$1" "$2")
  if [ -z "$got" ]; then
    bad "$6: browser $2 has no sampled pixel"
    grep -E "demo: (surface-sample|browser-surface) (browser=)?id?=?$2\b" "$1" |
      tail -4 | sed 's/^/         /'
    return
  fi
  read -r r g b <<<"$got"
  if near "$r" "$3" && near "$g" "$4" && near "$b" "$5"; then
    ok "$6: browser $2 sampled $r,$g,$b"
  else
    bad "$6: browser $2 sampled $r,$g,$b, wanted $3,$4,$5"
  fi
}

# Any sample of this id matches — for a browser whose surface is gone by the
# end of the run, when the last sample is the "none" that proves it.
expect_sample() { # expect_sample <logfile> <id> <r> <g> <b> <tag>
  local line rgb r g b
  while IFS= read -r line; do
    rgb=$(printf '%s\n' "$line" |
            sed -n 's/.*rgba=\([0-9]*\),\([0-9]*\),\([0-9]*\),[0-9]*$/\1 \2 \3/p')
    [ -n "$rgb" ] || continue
    read -r r g b <<<"$rgb"
    if near "$r" "$3" && near "$g" "$4" && near "$b" "$5"; then
      ok "$6: browser $2 sampled $r,$g,$b"
      return
    fi
  done < <(grep -E "^demo: surface-sample browser=$2 " "$1")
  bad "$6: browser $2 never sampled $3,$4,$5"
  grep -E "surface-sample browser=$2 " "$1" | tail -4 | sed 's/^/         /'
}

expect_none() { # expect_none <logfile> <id> <tag>
  local got
  got=$(sample_rgb "$1" "$2")
  if [ -n "$got" ]; then
    bad "$3: browser $2 still samples $got"
  elif grep -qE "^demo: surface-sample browser=$2 (rgba=none|none)$" "$1"; then
    ok "$3: browser $2 has no surface"
  else
    bad "$3: browser $2 was never sampled as none — see $1"
    grep -E "surface-(sample|size) browser=$2 " "$1" | tail -4 | sed 's/^/         /'
  fi
}

expect_surface() { # expect_surface <logfile> <id> <tag>
  if grep -qE "^demo: surface-(size|sample) browser=$2 .*size=[1-9][0-9]*x[1-9][0-9]*" "$1"; then
    ok "$3: browser $2 has a sized surface"
  else
    bad "$3: browser $2 has no sized surface — see $1"
    grep -E "surface-(sample|size) browser=$2 " "$1" | tail -4 | sed 's/^/         /'
  fi
}

# ---------------------------------------------------------------- scenario 1 --

# A plugin's popup and options pages open together. Ids are CEF's: the page is
# 1, the first new window 2, the next 3.
write_script same-open.script "load data:text/html,<title>pair</title>
wait 2500
new-window cefnest-extension://pair-a/popup.html
wait 800
new-window cefnest-extension://pair-a/options.html
wait 5000
surface-size 2
surface-sample 2
surface-size 3
surface-sample 3
quit"

run_demo same-open same-open.script --plugin-dir="$WORK/plugins"
log=$WORK/same-open.log
expect 'helper: plugins load id=pair-a root=' "$log" same-open
expect 'demo: browser 2 created \[window\] cefnest-extension://pair-a/popup.html' \
       "$log" same-open
expect 'demo: browser 3 created \[window\] cefnest-extension://pair-a/options.html' \
       "$log" same-open
expect 'demo: browser-surface id=2 size=' "$log" same-open
expect 'demo: browser-surface id=3 size=' "$log" same-open
expect_surface "$log" 2 same-open
expect_surface "$log" 3 same-open
expect_color "$log" 2 224 27 36 same-open
expect_color "$log" 3 28 113 216 same-open

# ---------------------------------------------------------------- scenario 2 --

# Close one, open another. The helper can hand the new window the surface
# address the closed one had; a pairing table with a stale key would skip the
# new toplevel and leave id 3 without a surface — or worse, keep id 2 paired
# to an address now drawing id 3's page.
write_script reuse.script "load data:text/html,<title>pair</title>
wait 2500
new-window cefnest-extension://pair-a/popup.html
wait 3500
surface-sample 2
close-browser 2
wait 2500
surface-sample 2
new-window cefnest-extension://pair-b/popup.html
wait 4500
surface-size 3
surface-sample 3
surface-sample 2
quit"

run_demo reuse reuse.script --plugin-dir="$WORK/plugins"
log=$WORK/reuse.log
expect 'demo: browser 2 created \[window\] cefnest-extension://pair-a/popup.html' \
       "$log" reuse
expect 'demo: browser-surface id=2 none' "$log" reuse
expect 'demo: browser 3 created \[window\] cefnest-extension://pair-b/popup.html' \
       "$log" reuse
expect 'demo: browser-surface id=3 size=' "$log" reuse
expect_sample "$log" 2 224 27 36 reuse
expect_surface "$log" 3 reuse
expect_color "$log" 3 46 194 126 reuse
# Browser 2 is gone, and its id stayed gone when the address came back.
expect_none "$log" 2 reuse

# ---------------------------------------------------------------- scenario 3 --

# The inspector is a windowed browser of its own — one more toplevel in the
# same order. It must not shift the pairing of the window opened after it.
write_script devtools.script "load file://$ROOT/test/menu-page.html
wait 2500
devtools-on
wait 500
click 600 400 273
wait 1500
menu-pick 26508
wait 5000
surface-size 2
new-window cefnest-extension://pair-c/popup.html
wait 4500
surface-size 2
surface-size 3
surface-sample 3
quit"

run_demo devtools devtools.script --plugin-dir="$WORK/plugins"
log=$WORK/devtools.log
expect 'demo: menu pick id=26508' "$log" devtools
expect 'helper: devtools browser 2 adopted' "$log" devtools
expect 'demo: browser 2 created \[window\] devtools=1' "$log" devtools
expect 'demo: browser-surface id=2 size=' "$log" devtools
expect_surface "$log" 2 devtools
expect 'demo: browser 3 created \[window\] cefnest-extension://pair-c/popup.html' \
       "$log" devtools
expect 'demo: browser-surface id=3 size=' "$log" devtools
expect_surface "$log" 3 devtools
expect_color "$log" 3 245 194 17 devtools

# ---------------------------------------------------------------- scenario 4 --

# Four plugins, four popup pages, four windows opened back to back. This is
# the order stress: every root comes from --plugin-dir, every page from the
# extension scheme, and every pixel must still answer for its own id.
write_script four.script "load data:text/html,<title>pair</title>
wait 2500
new-window cefnest-extension://pair-a/popup.html
wait 800
new-window cefnest-extension://pair-b/popup.html
wait 800
new-window cefnest-extension://pair-c/popup.html
wait 800
new-window cefnest-extension://pair-d/popup.html
wait 7000
surface-sample 2
surface-sample 3
surface-sample 4
surface-sample 5
quit"

run_demo four four.script --plugin-dir="$WORK/plugins"
log=$WORK/four.log
for id in pair-a pair-b pair-c pair-d; do
  expect "helper: plugins load id=$id root=" "$log" four
done
expect 'demo: browser 2 created \[window\] cefnest-extension://pair-a/popup.html' \
       "$log" four
expect 'demo: browser 3 created \[window\] cefnest-extension://pair-b/popup.html' \
       "$log" four
expect 'demo: browser 4 created \[window\] cefnest-extension://pair-c/popup.html' \
       "$log" four
expect 'demo: browser 5 created \[window\] cefnest-extension://pair-d/popup.html' \
       "$log" four
expect_surface "$log" 2 four
expect_surface "$log" 3 four
expect_surface "$log" 4 four
expect_surface "$log" 5 four
expect_color "$log" 2 224 27 36 four
expect_color "$log" 3 46 194 126 four
expect_color "$log" 4 245 194 17 four
expect_color "$log" 5 192 97 203 four

echo
if [ "$fails" = 0 ]; then
  printf 'surface-pair gate: all four scenarios passed\n'
else
  printf 'surface-pair gate: %d check(s) failed\n' "$fails"
  for f in same-open reuse devtools four; do
    [ -s "$WORK/$f.log" ] || continue
    echo "-- $f"; tail -40 "$WORK/$f.log" | sed 's/^/  /'
  done
  exit 1
fi
