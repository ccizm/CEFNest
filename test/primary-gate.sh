#!/usr/bin/env bash
#
# test/primary-gate.sh — page selection reaches the host primary clipboard.
#
# CEF on Wayland binds zwp_primary_selection_device_manager_v1 but does not
# create a source for a page selection. The helper injects a selection listener
# and pushes the text through a dedicated renderer message; the session writes it
# to Gtk's primary clipboard. This run selects "primary" in a textarea and reads
# the host primary back through CHROMIUM_PRIMARYWATCH.
#
#   ./test/primary-gate.sh
#   ./test/ci-session.sh ./test/primary-gate.sh
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/primary-page.html"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

cat >"$WORK/primary.session" <<SESSION
load $PAGE
wait 2200
drag 35 90 210 90
wait 1500
js-expr (()=>{const t=document.getElementById("t");return t.selectionStart+":"+t.selectionEnd+":"+t.value.substring(t.selectionStart,t.selectionEnd)})()
wait 500
quit
SESSION

echo "== page selection -> host primary"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_PRIMARYWATCH=1 \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/primary.session" \
  "$DEMO" >"$WORK/out" 2>"$WORK/err"
rc=$?
cat "$WORK/err" >>"$WORK/out"
if [ $rc != 0 ]; then bad "demo exited $rc"; fi
if grep -aq "demo: primary is 'primary'" "$WORK/out"; then
  ok "host primary is the selected word"
else
  bad "no 'demo: primary is primary' in the transcript"
  tail -15 "$WORK/out" | sed 's/^/         /'
fi
if grep -aq 'script 5 ok value="14:21:primary"' "$WORK/out"; then
  ok "the page selected 14:21 'primary'"
else
  bad "the page selection did not read back as 14:21 'primary'"
fi

echo
if [ $fails = 0 ]; then printf 'primary gate: all checks passed\n'
else printf 'primary gate: %d check(s) failed\n' "$fails"; exit 1; fi
