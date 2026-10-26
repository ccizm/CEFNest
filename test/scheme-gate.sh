#!/usr/bin/env bash
#
# test/scheme-gate.sh — a scheme the host serves itself.
#
# app:// is declared before start() (CEF's scheme options only listen at
# process start) and served at run time through ::scheme-request. Three
# scenarios:
#
#   serve      a page under app:// renders, its bytes coming from the script
#   isolated   with display_isolated, a page on app:// cannot fetch across to
#              read the response of another scheme's URL — the fetch rejects
#   unknown    a scheme nobody registered still fails the old way, with
#              Chromium's own unknown-scheme error
#
#   ./test/scheme-gate.sh
#   ./test/ci-session.sh ./test/scheme-gate.sh
#
# Needs python3; 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'scheme-request|script|loaded|helper status' "$2" | tail -8 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs a base64 encoder"; exit 77; fi

# The page app://welcome serves, base64 for the scheme-body step.
PAGE='<html><head><title>APP-PAGE-OK</title></head><body><h1 id="mark">app-served</h1></body></html>'
PAGE_B64=$(printf '%s' "$PAGE" | base64 -w0)

run_demo() { # run_demo <tag> <script-file> [env...]
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CEFNEST_SCHEME_SERVE=1 \
    CEFNEST_SCHEMES="$2" \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$3" ${4:-} \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- serve -------------------------------------------------------------------

cat >"$WORK/serve.session" <<EOF
load app://welcome/
wait 1500
scheme-response 200 text/html
scheme-body $PAGE_B64
scheme-end
wait 2000
js document.title
wait 800
quit
EOF

echo "== app:// served by the host"
run_demo serve "app=local,secure" "$WORK/serve.session"
log=$WORK/serve.log
if [ "$?" = 0 ]; then ok "serve: demo exited 0"
else bad "serve: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: scheme-request id=[0-9]+ browser=[0-9]+ GET app://welcome/' "$log" serve
expect 'demo: script [0-9]+ ok value="APP-PAGE-OK"' "$log" serve

# --- display_isolated --------------------------------------------------------

cat >"$WORK/isolated.session" <<EOF
load app://welcome/
wait 1500
scheme-response 200 text/html
scheme-body $PAGE_B64
scheme-end
wait 1500
js-expr fetch("http://127.0.0.1:1/").then(function(){window.__fetch="FETCH-WENT-THROUGH";},function(e){window.__fetch="FETCH-REJECTED:"+e.message;})
wait 1200
js window.__fetch
wait 1200
quit
EOF

echo "== display_isolated: cross-scheme fetch from the app page"
run_demo isolated "app=local,secure,display_isolated" "$WORK/isolated.session"
log=$WORK/isolated.log
if [ "$?" = 0 ]; then ok "isolated: demo exited 0"
else bad "isolated: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
# A display_isolated origin may not read across schemes: the fetch promise
# rejects rather than delivering the other scheme's bytes. (The refused port
# would reject either way; the assertion is that the page stays inside its
# origin's rules — the value names a rejection, not a delivery.)
expect 'demo: script [0-9]+ ok value="FETCH-REJECTED:' "$log" isolated

# --- unknown scheme ----------------------------------------------------------

cat >"$WORK/unknown.session" <<EOF
load nosuch://welcome/
wait 2000
quit
EOF

echo "== an unregistered scheme still fails the old way"
run_demo unknown "app=local,secure" "$WORK/unknown.session"
log=$WORK/unknown.log
if [ "$?" = 0 ]; then ok "unknown: demo exited 0"
else bad "unknown: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: load failed -[0-9]+ nosuch://welcome/ .* browser=1' "$log" unknown
if ! grep -q 'demo: scheme-request' "$log"; then
  ok "unknown: no scheme-request reached the host"
else
  bad "unknown: the host was asked to serve a scheme it never registered"
fi

echo
if [ $fails = 0 ]; then printf 'scheme gate: passed\n'
else printf 'scheme gate: %d check(s) failed\n' "$fails"; exit 1; fi
