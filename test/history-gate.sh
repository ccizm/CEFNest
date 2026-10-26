#!/usr/bin/env bash
#
# test/history-gate.sh — navigation history and the load lifecycle.
#
# Three runs grade the B batch:
#
#   history     two pages, one history request; the second page's entry must
#               be the current one and both titles must be there
#   lifecycle   a success page, a 404 and a connection-refused host each get
#               exactly one ::load-started and exactly one end event, and a
#               failure reports exactly one ::load-error (no ERR_ABORTED echo)
#   history-go  three pages, then history-go 0 lands the browser back on the
#               first one — the jump CEF's public API has no button for
#
#   ./test/history-gate.sh
#   ./test/ci-session.sh ./test/history-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
FIRST="file://$ROOT/test/browser-data-page.html"
SECOND="file://$ROOT/test/history-second-page.html"
THIRD="file://$ROOT/test/find-page.html"
DEAD="http://127.0.0.1:1/"

WORK=$(mktemp -d)
HTTPD=
trap '[ -n "$HTTPD" ] && kill "$HTTPD" 2>/dev/null; rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'history|loaded|load-started|load failed|helper: ' "$2" | tail -10 | sed 's/^/         /'
  fi
}
expect_count() { # expect_count <pattern> <n> <logfile> <tag>
  local got
  got=$(grep -cE "$1" "$3")
  if [ "$got" = "$2" ]; then ok "$4: $2 of /$1/"
  else bad "$4: wanted $2 of /$1/, got $got"; fi
}
expect_none() { # expect_none <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs a 404 server"; exit 77; fi

# The 404 server: python's own file handler refuses what is not there.
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
(cd "$WORK" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) \
  >"$WORK/httpd.log" 2>&1 &
HTTPD=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done
NOT_FOUND="http://127.0.0.1:$PORT/no-such-page.html"

# --- history -----------------------------------------------------------------

cat >"$WORK/history.session" <<EOF
load $FIRST
wait 1800
load $SECOND
wait 1800
history
wait 2500
close
EOF

echo "== history"
timeout --kill-after=10s 60 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/history.session" \
  "$DEMO" >"$WORK/history.log" 2>&1
status=$?
log=$WORK/history.log
if [ "$status" = 0 ]; then ok "history: demo exited 0"
else bad "history: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: history id=[0-9]+ json=.*"current_index":1' "$log" current
expect 'demo: history id=[0-9]+ json=.*"index":0,"title":"Browser data"' "$log" first entry
expect 'demo: history id=[0-9]+ json=.*"index":1,"title":"History second page"' "$log" second entry
expect 'demo: history id=[0-9]+ json=.*"current":true' "$log" current flag

if [ "$(grep -c 'demo: history id=' "$log")" = 1 ]; then
  ok "history: one request, one answer"
else
  bad "history: $(grep -c 'demo: history id=' "$log") answers for one request"
fi

# --- lifecycle ---------------------------------------------------------------

cat >"$WORK/lifecycle.session" <<EOF
load $FIRST
wait 1500
load $NOT_FOUND
wait 1500
load $DEAD
wait 2500
close
EOF

echo "== lifecycle: success, 404, refused"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/lifecycle.session" \
  "$DEMO" >"$WORK/lifecycle.log" 2>&1
status=$?
log=$WORK/lifecycle.log
if [ "$status" = 0 ]; then ok "lifecycle: demo exited 0"
else bad "lifecycle: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# Success: one started, one end, no error.
expect_count 'demo: load-started browser=[0-9]+ .*browser-data-page' 1 "$log" success started
expect_count 'demo: loaded .*browser-data-page\.html \(HTTP 200\) browser=[0-9]+' 1 "$log" success ended
expect_none "demo: load failed.*browser-data-page" "$log" success error

# 404: the navigation commits, so the end carries the HTTP code and no error.
expect_count 'demo: loaded .*no-such-page.html \(HTTP 404\) browser=[0-9]+' 1 "$log" 404 ended
expect_none "demo: load failed.*no-such-page" "$log" 404 error

# Refused: exactly one real error (never the ERR_ABORTED echo) and one end.
expect_count 'demo: load failed -[0-9]+ '"$DEAD"' .* browser=[0-9]+' 1 "$log" refused error
expect_count 'demo: loaded .*http://127.0.0.1:1/ \(HTTP 0\) browser=[0-9]+' 1 "$log" refused ended
expect_none 'demo: load failed.*ERR_ABORTED' "$log" no aborted echo

# --- history-go --------------------------------------------------------------

cat >"$WORK/history-go.session" <<EOF
load $FIRST
wait 1500
load $SECOND
wait 1500
load $THIRD
wait 1500
history-go 0
wait 2500
close
EOF

echo "== history-go: back to the first of three"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/history-go.session" \
  "$DEMO" >"$WORK/history-go.log" 2>&1
status=$?
log=$WORK/history-go.log
if [ "$status" = 0 ]; then ok "history-go: demo exited 0"
else bad "history-go: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'helper: history-go browser=[0-9]+ index=0 done' "$log" go: the jump ran
expect 'demo: loaded .*browser-data-page\.html \(HTTP 200\) browser=[0-9]+' "$log" go: the first page reloaded
expect_none 'helper status: history-go-failed' "$log" go: no failure status

echo
if [ $fails = 0 ]; then printf 'history gate: all checks passed\n'
else printf 'history gate: %d check(s) failed\n' "$fails"; exit 1; fi
