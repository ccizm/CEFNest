#!/usr/bin/env bash
#
# test/cookies-gate.sh — the cookie commands, read back from the page itself.
#
# Cookies need an http origin (Chromium refuses document.cookie on file://), so
# this gate serves test/ over loopback and drives the real helper:
#
#   get     an empty jar answers cookies:[] count:0 — the path with no visitor
#           callback at all, which is why the helper has a delayed answer
#   set     one cookie answers count:1 failed:[]
#   page    document.cookie sees probe=42, so the value reached Chromium, not
#           just the helper's own table
#   get     the same jar now answers the cookie with its fields
#   delete  by name answers deleted:1
#   page    document.cookie is empty again
#   get     and the jar agrees
#
#   ./test/cookies-gate.sh
#   ./test/ci-session.sh ./test/cookies-gate.sh
#
# Needs python3 (the HTTP server); 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
SERVER=
cleanup() { [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: cookies|demo: script|loaded|helper: ' "$2" | tail -12 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then
  echo "no python3 — the cookie origin needs an HTTP server"; exit 77
fi

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$ROOT/test" \
  >"$WORK/http.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done

URL="http://127.0.0.1:$PORT/browser-data-page.html"
ORIGIN="http://127.0.0.1:$PORT/"

cat >"$WORK/cookies.session" <<EOF
load $URL
wait 2000
cookies-get $ORIGIN
wait 2200
cookies-set $ORIGIN probe 42
wait 1500
js document.cookie
wait 800
cookies-get $ORIGIN
wait 2200
cookies-delete $ORIGIN probe
wait 1500
js document.cookie
wait 800
cookies-get $ORIGIN
wait 2200
close
EOF

echo "== cookies over $ORIGIN"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/cookies.session" \
  "$DEMO" >"$WORK/cookies.log" 2>&1
status=$?
log=$WORK/cookies.log
if [ "$status" = 0 ]; then ok "cookies: demo exited 0"
else bad "cookies: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect "demo: loaded $URL \(HTTP 200\)" "$log" page
expect 'demo: cookies id=[0-9]+ ok=1 json=.*"cookies":\[\],"count":0' "$log" "empty jar"
expect 'demo: cookies id=[0-9]+ ok=1 json=.*"count":1.*"failed":\[\]' "$log" "set accepted"
expect 'demo: script [0-9]+ ok value="probe=42"' "$log" "page sees the cookie"
expect 'demo: cookies id=[0-9]+ ok=1 json=.*"name":"probe".*"value":"42".*"count":1' \
  "$log" "get returns it"
expect 'demo: cookies id=[0-9]+ ok=1 json=.*"deleted":1' "$log" "delete by name"
expect 'demo: script [0-9]+ ok value=""' "$log" "page sees the deletion"
expect 'demo: cookies id=[0-9]+ ok=1 json=.*"cookies":\[\],"count":0' "$log" "jar empty again"

# The empty-jar answer is the helper's delayed path; if it never fired the host
# would hang. The gate's waits above only pass if all three arrived.
count=$(grep -c 'demo: cookies .*"cookies":' "$log" || true)
if [ "$count" = 3 ]; then ok "cookies: three gets answered, nothing hung"
else bad "cookies: $count of 3 gets answered"; fi

echo
if [ $fails = 0 ]; then printf 'cookies gate: all checks passed\n'
else printf 'cookies gate: %d check(s) failed\n' "$fails"; exit 1; fi
