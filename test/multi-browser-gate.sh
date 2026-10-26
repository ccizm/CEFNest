#!/usr/bin/env bash
#
# test/multi-browser-gate.sh — one helper, a tab and a standalone window.
#
# The tab is a second CefBrowserView in the shared CefWindow; the standalone
# window is a second CefWindow. The demo creates a second GtkWindow for the
# latter and binds it to that toplevel surface. This gate stays at the protocol
# level: it asserts both browser ids, the tab/window creation, closing one tab
# without taking the other with it, and the separate host window; the pixel
# checks live in the screenshots taken during development.
#
#   ./test/multi-browser-gate.sh
#   ./test/ci-session.sh ./test/multi-browser-gate.sh
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/menu-page.html"
TAB_PAGE="file://$ROOT/test/spellcheck-page.html"
THIRD="file://$ROOT/test/find-page.html"
WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
trap '[ "$KEEP" = 1 ] && echo "  note: transcripts kept in $WORK" || rm -rf "$WORK"' 

fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

cat >"$WORK/multi.session" <<SESSION
load $PAGE
wait 2200
new-tab $TAB_PAGE
wait 2200
js-expr document.body.innerText.includes("spellcheck")
wait 800
close-browser 2
wait 1500
js-expr document.title.includes("MENU")
wait 800
new-window $PAGE
wait 2200
quit
SESSION

echo "== one helper: tab + standalone window"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$HELPER" \
  CHROMIUM_SESSION_SCRIPT="$WORK/multi.session" "$DEMO" \
  >"$WORK/out" 2>"$WORK/err"
rc=$?
cat "$WORK/err" >>"$WORK/out"
if [ $rc != 0 ]; then bad "demo exited $rc"; fi

expect() {
  if grep -aqE "$1" "$WORK/out"; then ok "$2"; else
    bad "$2"
    tail -20 "$WORK/out" | sed 's/^/         /'
  fi
}

# The later scenarios have their own logs; the same assertion, a file argument.
expect_in() { # expect_in <logfile> <pattern> <tag>
  if grep -aqE "$2" "$1"; then ok "$3"; else
    bad "$3"
    tail -20 "$1" | sed 's/^/         /'
  fi
}

expect 'browser created, id=1 ' 'first browser created'
expect 'browser created, id=2 ' 'second browser created'
expect 'new tab slot 2 in tab window' 'second browser is a tab'
# Closing one tab has to close that browser and leave the one underneath:
# all three lines below are absent when the detach/close order is wrong.
expect 'helper: browser 2 closing' 'the tab really reached OnBeforeClose'
expect 'demo: browser 2 closed' 'the host was told the tab closed'
expect 'demo: browser 1 selected' 'the remaining tab became active again'
expect 'browser 3 standalone window' 'third browser got a host window'
expect 'new window slot 3' 'third browser is a standalone CefWindow'
expect 'script 5 ok value=true' 'active tab is the spellcheck page'
expect 'script 9 ok value=true' 'the remaining tab still answers after the close'


# --- A.1: a background tab navigates on its own ------------------------------

cat >"$WORK/inactive.session" <<SESSION
load $PAGE
wait 2000
new-tab $TAB_PAGE
wait 2000
select-browser 1
wait 800
load-browser 2 $THIRD
wait 3000
back-browser 2
wait 2500
forward-browser 2
wait 2500
quit
SESSION

echo "== navigation aimed at a background tab"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$HELPER" \
  CHROMIUM_SESSION_SCRIPT="$WORK/inactive.session" "$DEMO" \
  >"$WORK/out2" 2>"$WORK/err2"
rc=$?
cat "$WORK/err2" >>"$WORK/out2"
if [ $rc != 0 ]; then bad "inactive: demo exited $rc"; fi

expect_in "$WORK/out2" 'browser-row id=2 url=.*find-page.html .*active=0' 'the background tab took the third page'
expect_in "$WORK/out2" 'browser-row id=2 url=.*spellcheck-page.html .*active=0' 'back-browser 2 went back'
expect_in "$WORK/out2" 'browser-row id=1 url=.*menu-page.html' 'browser 1 never moved'

# --- A.3: the popup question carries disposition, gesture and browser --------

POPUP_PAGE="file://$ROOT/test/popup-gate-page.html"

cat >"$WORK/popup.session" <<SESSION
load $POPUP_PAGE
wait 2500
click 200 150 274
wait 1500
click 200 150 272
wait 2000
quit
SESSION

echo "== popup-requested: middle click vs target=_blank"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$HELPER" \
  CHROMIUM_SESSION_SCRIPT="$WORK/popup.session" "$DEMO" \
  >"$WORK/out3" 2>"$WORK/err3"
rc=$?
cat "$WORK/err3" >>"$WORK/out3"
if [ $rc != 0 ]; then bad "popup: demo exited $rc"; fi

expect_in "$WORK/out3" 'demo: popup-requested browser=1 .*popup-target.html disposition=[0-9]+ gesture=1' \
       'both asks name the asking browser and the gesture'
BG=$(grep -aoE 'popup-requested browser=1 [^ ]* disposition=[0-9]+' "$WORK/out3" |
     grep -oE 'disposition=[0-9]+' | sort -u | wc -l)
if [ "$BG" -ge 2 ]; then ok 'the two dispositions differ'
else bad "the two dispositions differ ($(grep -a 'popup-requested' "$WORK/out3" | head -2 | tr '\n' ' '))"; fi

echo
if [ $fails = 0 ]; then printf 'multi-browser gate: all checks passed\n'
else printf 'multi-browser gate: %d check(s) failed\n' "$fails"; exit 1; fi
