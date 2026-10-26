#!/usr/bin/env bash
#
# test/accept-language-gate.sh — asserts that the accept-language property
# reaches the renderer.
#
# CEF fixes the language list when the context is created, so the property is
# a start-time one like proxy: the session passes it to the helper's
# environment and the helper stores it in CefSettings before CefInitialize.
#
# The probe reads navigator.language (what a page sees) and the helper log line
# proves the value arrived from the host, not from the test machine's locale.
#
#   ./test/accept-language-gate.sh
#   ./test/ci-session.sh ./test/accept-language-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then echo "  note: transcripts kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'accept-language|script [0-9]+' "$2" | head -10 | sed 's/^/         /'
  fi
}

js_value() { # js_value <logfile> <n>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

cat > "$WORK/lang.session" <<'EOF'
load data:text/html,<title>lang</title><p>lang</p>
wait 2000
js-expr navigator.language
wait 400
js-expr navigator.languages.join(",")
wait 400
close
EOF

echo "== accept-language"
timeout --kill-after=10s 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$HELPER" CEFNEST_ACCEPT_LANG="zh-CN,zh;q=0.9,en;q=0.8" \
  CHROMIUM_SESSION_SCRIPT="$WORK/lang.session" \
  "$DEMO" >"$WORK/lang.log" 2>&1
status=$?
log=$WORK/lang.log
if [ "$status" = 0 ]; then ok "language: demo exited 0"
else bad "language: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'helper: accept-language=zh-CN,zh;q=0\.9,en;q=0\.8' "$log" language
[ "$(js_value "$log" 1)" = "zh-CN" ] \
  && ok "language: navigator.language is zh-CN" \
  || bad "language: navigator.language was '$(js_value "$log" 1)'"
case "$(js_value "$log" 2)" in
  zh-CN*) ok "language: navigator.languages starts with zh-CN" ;;
  *) bad "language: navigator.languages was '$(js_value "$log" 2)'" ;;
esac

echo
if [ $fails = 0 ]; then printf 'accept-language gate: all checks passed\n'
else printf 'accept-language gate: %d check(s) failed\n' "$fails"; exit 1; fi
