#!/usr/bin/env bash
#
# test/devtools-gate.sh — the inspector switch, end to end.
#
# Six scripted runs against the real CEF helper and the demo:
#
#   1. default off      the context menu arrives without the 检查 row and the
#                       helper never hears the switch
#   2. --devtools       the row is there; picking it makes the helper adopt a
#                       devtools browser and the demo open a standalone window,
#                       and the window gets a surface from the nested compositor
#   3. close devtools   close-browser on the inspector closes that browser only;
#                       the page stays up
#   4. close page       closing the page takes the inspector with it (CEF's
#                       association), and the helper reports both closes
#   5. runtime off      --devtools, then devtools-off before the click: the next
#                       menu has no 检查 row
#   6. runtime on       no flag, then devtools-on while the helper runs: the row
#                       appears and picking it opens the inspector
#
# The menu path needs gtk_popover_popup() to return, and the headless mutter
# ci-session.sh provides does not do that (measured 2026-10-22, see menu-gate.sh).
# Rather than skip the whole gate, a headless run re-executes itself under Xvfb:
# the demo's own nested compositor still hosts Chromium, so what is being tested
# is unchanged, and the host's popover gets an X11 server that maps it.
#
#   ./test/devtools-gate.sh
#   ./test/ci-session.sh ./test/devtools-gate.sh   # re-executes under Xvfb
#   KEEP_LOGS=1 ./test/devtools-gate.sh
#
# Nothing here touches the pointer or the keyboard of the desktop: a right click
# is injected into the demo's nested compositor. On a real desktop the runs do
# open windows, which is the convention for a manual gate here.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/menu-page.html"
. "$ROOT/test/helper-path.sh"

if [ "${CEFNEST_HEADLESS:-0}" = 1 ] && [ -z "${DEVTOOLS_GATE_XVFB:-}" ]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    export DEVTOOLS_GATE_XVFB=1
    exec xvfb-run -a env -u WAYLAND_DISPLAY GDK_BACKEND=x11 "$0" "$@"
  fi
  printf '  skip devtools: no xvfb-run, and headless mutter does not return from gtk_popover_popup()\n'
  printf 'devtools gate: skipped, it could not assert anything\n'
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
# in XDG_RUNTIME_DIR; give them one nobody else reads.
mkdir -p "$WORK/xdg"
chmod 700 "$WORK/xdg"
export XDG_RUNTIME_DIR="$WORK/xdg"

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
  CHROMIUM_SESSION_SCRIPT=$WORK/$script CHROMIUM_HELPER=$HELPER \
    timeout --kill-after=10s 90 "$DEMO" "$@" </dev/null >"$WORK/$tag.log" 2>&1
  RC=$?
  if [ "$RC" = 0 ]; then ok "$tag: demo exited 0"
  else bad "$tag: demo exited $RC"; tail -20 "$WORK/$tag.log" | sed 's/^/         /'; fi
}

# The click opens the session's own popover; menu-dismiss answers it, and
# menu-pick 26508 is the 检查 row. Coordinates are the page view's own.
write_script() { # write_script <file> <body>
  printf '%s\n' "$2" > "$WORK/$1"
}

write_script off.script "load $PAGE
wait 2000
click 600 400 273
wait 1500
menu-dismiss
wait 500
quit"

write_script open.script "load $PAGE
wait 2000
click 600 400 273
wait 1500
menu-pick 26508
wait 5000
quit"

write_script close-devtools.script "load $PAGE
wait 2000
click 600 400 273
wait 1500
menu-pick 26508
wait 5000
close-browser 2
wait 2500
quit"

write_script close-page.script "load $PAGE
wait 2000
click 600 400 273
wait 1500
menu-pick 26508
wait 5000
close-browser 1
wait 3000
quit"

write_script runtime-off.script "load $PAGE
wait 2000
devtools-off
wait 500
click 600 400 273
wait 1500
menu-dismiss
wait 500
quit"

write_script runtime-on.script "load $PAGE
wait 2000
devtools-on
wait 500
click 600 400 273
wait 1500
menu-pick 26508
wait 5000
quit"

# 1. Default off.
run_demo off off.script
log=$WORK/off.log
expect 'demo: context menu at 600,400 with [0-9]+ rows' "$log" off
expect_no 'id=26508' "$log" off
expect_no 'helper: devtools enabled' "$log" off
expect_no 'devtools browser' "$log" off
expect_no 'standalone window' "$log" off

# 2. Switched on: the row, the adoption, the standalone window and its surface.
run_demo open open.script --devtools
log=$WORK/open.log
expect 'helper: devtools enabled' "$log" on
expect 'demo: menu row [0-9]+ id=26508 type=command enabled=1 checked=0 label=检查' \
       "$log" on
# The row belongs at the bottom of the model, under the browser's own items —
# Chrome's own menu does the same.
last_row=$(grep -E '^demo: menu row [0-9]+ id=' "$log" | tail -1)
case "$last_row" in
  *'id=26508 '*) ok "on: the 检查 row is the last menu row" ;;
  *) bad "on: the 检查 row is not last, got: $last_row" ;;
esac
expect 'demo: menu pick id=26508' "$log" on
expect 'helper: devtools browser 2 adopted' "$log" on
expect 'helper: devtools popup in a frameless window' "$log" on
expect 'helper: browser created, id=2 slot=2 windowed=1 devtools=1' "$log" on
expect 'demo: browser 2 created \[window\] devtools=1' "$log" on
expect 'demo: browser 2 standalone window \(devtools\)' "$log" on
expect 'demo: browser 2 got a surface' "$log" on

# 3. Closing the inspector leaves the page running.
run_demo close-devtools close-devtools.script --devtools
log=$WORK/close-devtools.log
expect 'helper: browser 2 closing' "$log" close-dev
expect 'demo: browser 2 closed' "$log" close-dev
expect_no 'demo: browser 1 closed' "$log" close-dev
expect_no 'demo: the helper went away' "$log" close-dev

# 4. Closing the page closes the inspector with it. The helper then has no
# browser left and exits, so the run ends on ::closed — exit 1 with the reason
# is the correct outcome, not a failure to finish.
echo "== close-page"
CHROMIUM_SESSION_SCRIPT=$WORK/close-page.script CHROMIUM_HELPER=$HELPER \
  timeout --kill-after=10s 90 "$DEMO" --devtools </dev/null >"$WORK/close-page.log" 2>&1
RC=$?
log=$WORK/close-page.log
if [ "$RC" = 1 ]; then ok "close-page: demo exited 1 (the helper closed itself)"
else bad "close-page: demo exited $RC, 1 wanted"; fi
expect 'helper: browser 1 closing' "$log" close-page
expect 'demo: browser 1 closed' "$log" close-page
expect 'helper: browser 2 closing' "$log" close-page
expect 'demo: browser 2 closed' "$log" close-page
expect 'demo: the helper went away \(helper closed the browser\)' "$log" close-page

# 5. On, then off while the helper runs.
run_demo runtime-off runtime-off.script --devtools
log=$WORK/runtime-off.log
expect 'helper: devtools enabled' "$log" run-off
expect 'helper: devtools disabled' "$log" run-off
expect 'demo: context menu at 600,400 with [0-9]+ rows' "$log" run-off
expect_no 'id=26508' "$log" run-off
expect_no 'devtools browser' "$log" run-off

# 6. Off, then on while the helper runs.
run_demo runtime-on runtime-on.script
log=$WORK/runtime-on.log
expect 'helper: devtools disabled' "$log" run-on
expect 'helper: devtools enabled' "$log" run-on
expect 'demo: menu row [0-9]+ id=26508' "$log" run-on
expect 'helper: devtools browser 2 adopted' "$log" run-on
expect 'helper: devtools popup in a frameless window' "$log" run-on
expect 'demo: browser 2 standalone window \(devtools\)' "$log" run-on

echo
if [ "$fails" = 0 ]; then
  printf 'devtools gate: passed\n'
else
  printf 'devtools gate: %d check(s) failed\n' "$fails"
  for f in off open close-devtools close-page runtime-off runtime-on; do
    [ -s "$WORK/$f.log" ] || continue
    echo "-- $f"; tail -40 "$WORK/$f.log" | sed 's/^/  /'
  done
  exit 1
fi
