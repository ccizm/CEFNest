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
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
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

echo
if [ $fails = 0 ]; then printf 'multi-browser gate: all checks passed\n'
else printf 'multi-browser gate: %d check(s) failed\n' "$fails"; exit 1; fi
