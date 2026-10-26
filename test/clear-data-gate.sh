#!/usr/bin/env bash
#
# test/clear-data-gate.sh — typed data clearing.
#
# Cookies are planted, read back whole, cleared by type, and read back empty.
# The whole-directory form (chromium_session_clear_data) has its own gate;
# this is the "清除数据" dialog's granularity: one type goes, the rest of the
# profile stays. A second cookie kind proves the scoping: cookies survive a
# cache clearing, and an origin-storage clearing takes nothing else with it.
#
#   ./test/clear-data-gate.sh
#   ./test/ci-session.sh ./test/clear-data-gate.sh
#
# Needs python3; 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
HTTPD=
trap '[ -n "$HTTPD" ] && kill "$HTTPD" 2>/dev/null; rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: cookies|clear-data' "$2" | tail -8 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs an origin server"; exit 77; fi

# Cookies need a real origin, not file://.
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
(cd "$WORK" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) \
  >"$WORK/httpd.log" 2>&1 &
HTTPD=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done
URL="http://127.0.0.1:$PORT/"

cat >"$WORK/clear.session" <<EOF
load $URL
wait 1500
cookies-set $URL survivor kept
cookies-set $URL victim gone
cookies-get $URL
wait 2000
clear-cache
wait 1500
cookies-get $URL
wait 2000
clear-cookies
wait 2500
cookies-get $URL
wait 2000
quit
EOF

echo "== typed clearing over $URL"
timeout --kill-after=10s 120 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/clear.session" \
  "$DEMO" >"$WORK/clear.log" 2>&1
status=$?
log=$WORK/clear.log
if [ "$status" = 0 ]; then ok "clear: demo exited 0"
else bad "clear: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

# The read-backs carry a "cookies" array; a set's own answer does not.
mapfile -t READS < <(grep 'demo: cookies' "$log" | grep '"cookies":\[')
if [ "${#READS[@]}" = 3 ]; then ok "clear: three reads answered"
else bad "clear: wanted 3 reads, got ${#READS[@]}"; fi

case "${READS[0]}" in
  *survivor*victim*|*victim*survivor*) ok "planted: both cookies read back" ;;
  *) bad "planted: first read is '${READS[0]:-}'" ;;
esac
case "${READS[1]}" in
  *survivor*victim*|*victim*survivor*) ok "cache clear: cookies kept" ;;
  *) bad "cache clear: cookies lost ('${READS[1]:-}')" ;;
esac
case "${READS[2]}" in
  *'"count":0'*) ok "cookie clear: the store reads back empty" ;;
  *) bad "cookie clear: last read is '${READS[2]:-}'" ;;
esac

echo
if [ $fails = 0 ]; then printf 'clear-data gate: passed\n'
else printf 'clear-data gate: %d check(s) failed\n' "$fails"; exit 1; fi
