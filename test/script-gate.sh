#!/usr/bin/env bash
#
# test/script-gate.sh — the host's own scripts in the page.
#
#   document_start  a script registered for document_start exists before the
#                   page's own inline script runs — the same timing the plugin
#                   content scripts measured
#   patterns        a script whose pattern names one file runs there and
#                   nowhere else
#   per-browser     a script scoped to one browser does not reach another
#
#   ./test/script-gate.sh
#   ./test/ci-session.sh ./test/script-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/script-gate-page.html"
OTHER="file://$ROOT/test/pixel-page.html"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
trap '[ "$KEEP" = 1 ] && echo "  note: transcripts kept in $WORK" || rm -rf "$WORK"'

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: script|script-add|loaded' "$2" | tail -8 | sed 's/^/         /'
  fi
}
b64() { printf '%s' "$1" | base64 -w0; }

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

# --- document_start ----------------------------------------------------------

cat >"$WORK/start.session" <<EOF
script-add 1 document_start <all_urls> $(b64 'window.__hostStart = 1;')
load $PAGE
wait 1800
js window.__inlineSaw + ":" + String(window.__hostStart)
wait 800
quit
EOF

echo "== document_start runs before the page's inline script"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/start.session" \
  "$DEMO" >"$WORK/start.log" 2>&1
log=$WORK/start.log
if [ "$?" = 0 ]; then ok "start: demo exited 0"
else bad "start: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: script [0-9]+ ok value="yes:1"' "$log" start

# --- patterns ----------------------------------------------------------------

cat >"$WORK/pattern.session" <<EOF
script-add 1 document_idle file://*/*script-gate-page.html $(b64 'window.__hit = "hit";')
load $OTHER
wait 2200
js String(window.__hit)
wait 900
load $PAGE
wait 2200
js String(window.__hit)
wait 900
quit
EOF

echo "== pattern hit and miss"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/pattern.session" \
  "$DEMO" >"$WORK/pattern.log" 2>&1
log=$WORK/pattern.log
if [ "$?" = 0 ]; then ok "pattern: demo exited 0"
else bad "pattern: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
# First value: the page the pattern does not name. Second: the one it does.
expect 'demo: script [0-9]+ ok value="undefined"' "$log" pattern: miss
expect 'demo: script [0-9]+ ok value="hit"' "$log" pattern: hit

# --- per-browser -------------------------------------------------------------

cat >"$WORK/scoped.session" <<EOF
script-add-for 7 1 document_idle file://*/*pixel-page.html $(b64 'window.__scoped = "scoped";')
load $OTHER
wait 1500
new-tab $OTHER
wait 2000
select-browser 1
wait 800
js String(window.__scoped)
wait 900
select-browser 2
wait 800
js String(window.__scoped)
wait 900
quit
EOF

echo "== per-browser scope"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/scoped.session" \
  "$DEMO" >"$WORK/scoped.log" 2>&1
log=$WORK/scoped.log
if [ "$?" = 0 ]; then ok "scoped: demo exited 0"
else bad "scoped: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: script [0-9]+ ok value="scoped"' "$log" scoped: browser 1 has it
expect 'demo: script [0-9]+ ok value="undefined"' "$log" scoped: browser 2 does not

echo
if [ $fails = 0 ]; then printf 'script gate: all checks passed\n'
else printf 'script gate: %d check(s) failed\n' "$fails"; exit 1; fi
