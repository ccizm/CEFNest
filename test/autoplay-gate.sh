#!/usr/bin/env bash
#
# test/autoplay-gate.sh — the autoplay policy reaches Chromium.
#
# The probe page builds an AudioContext the moment it loads. Measured on this
# Chromium (156): the *default* policy already suspends a gesture-less
# AudioContext, `user-gesture-required` does not govern Web Audio at all, and
# the pair that flips the state both ways is
# document-user-activation-required (suspended) vs
# no-user-gesture-required (running). The gate asserts that pair — both
# directions of the wiring, property to env to switch to Chromium.
#
#   ./test/autoplay-gate.sh
#   ./test/ci-session.sh ./test/autoplay-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/autoplay-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

js_value() { # js_value <logfile>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | tail -1
}

run() { # run <logfile> <policy>
  local log=$1 policy=$2
  cat >"$WORK/autoplay.session" <<EOF
load $PAGE
wait 1200
js window.__actState
wait 600
quit
EOF
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/autoplay.session" \
    ${policy:+CEFNEST_AUTOPLAY_POLICY=$policy} \
    "$DEMO" >"$log" 2>&1
}

assert_state() { # assert_state <logfile> <wanted> <tag> <flagline>
  local value
  value=$(js_value "$1")
  if [ "$value" = "$2" ]; then ok "$3: AudioContext $2"
  else bad "$3: wanted $2, got '$value'"; fi
  grep -qE "helper: --autoplay-policy=$4" "$1" &&
    ok "$3: helper appended --autoplay-policy=$4" ||
    bad "$3: no helper log line for --autoplay-policy=$4"
}

echo "== document-user-activation-required: AudioContext suspended"
run "$WORK/strict.log" document-user-activation-required
if [ $? = 0 ]; then ok "strict: demo exited 0"
else bad "strict: demo exited $?"; fi
assert_state "$WORK/strict.log" suspended strict document-user-activation-required

echo "== no-user-gesture-required: AudioContext running"
run "$WORK/free.log" no-user-gesture-required
if [ $? = 0 ]; then ok "free: demo exited 0"
else bad "free: demo exited $?"; fi
assert_state "$WORK/free.log" running free no-user-gesture-required

echo
if [ $fails = 0 ]; then printf 'autoplay gate: all checks passed\n'
else printf 'autoplay gate: %d check(s) failed\n' "$fails"; exit 1; fi
