#!/usr/bin/env bash
#
# test/file-dialog-gate.sh — Chromium's file chooser, answered by the host.
#
# Three scripted runs against the real CEF helper and the demo:
#
#   1. answer    a single file through the open dialog, then two files through
#                the multiple one; the page reads both selections back
#   2. cancel    an explicit empty answer, which is the cancel button
#   3. default   the host declines the signal: the session cancels, and the
#                page hears the input's own cancel event
#
# Measured while building this: HTMLInputElement.click() from page script is
# ignored for a file input — Chromium wants transient activation — so the gate
# clicks the input through the demo's nested compositor. That click never
# touches the desktop pointer.
#
#   ./test/file-dialog-gate.sh
#   ./test/ci-session.sh ./test/file-dialog-gate.sh
#   KEEP_LOGS=1 ./test/file-dialog-gate.sh
#
# Both streams go through tee for the reason permission-gate.sh documents: the
# session's debug lines are half the contract and GLib sends them to stdout once
# the process is off a tty.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/file-dialog-page.html"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then
    echo "  note: transcripts kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

# The chooser hands paths to the renderer, which makes File objects out of them;
# the fixtures are real files so the selection is the one a person could make.
printf 'chosen\n' > "$WORK/chosen.txt"
printf 'one\n' > "$WORK/one.txt"
printf 'two\n' > "$WORK/two.txt"

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'file-dialog|script [0-9]+' "$2" | head -10 | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_after() { # expect_after <earlier> <later> <logfile> <tag>
  local a b
  a=$(grep -nE "$1" "$3" | head -1 | cut -d: -f1)
  b=$(grep -nE "$2" "$3" | head -1 | cut -d: -f1)
  if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ]; then
    ok "$4: /$2/ (line $b) came after /$1/ (line $a)"
  else
    bad "$4: wanted /$2/ after /$1/ — got '${b:-none}' and '${a:-none}'"
  fi
}

js_value() { # js_value <logfile> <n>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

run_demo() { # run_demo <tag> <logfile> <extra env...>
  local tag=$1 log=$2
  shift 2

  if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
  if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
  echo "== $tag"
  timeout --kill-after=10s 90 env "$@" G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/$tag.session" \
    "$DEMO" 2>&1 | tee "$log" | cat > /dev/null
  local status=${PIPESTATUS[0]}
  if [ "$status" = 0 ]; then ok "$tag: demo exited 0"
  else
    bad "$tag: demo exited $status"
    tail -8 "$log" | sed 's/^/         /'
  fi
}

script() { # script <tag> — writes the heredoc that follows into the tag's script
  cat > "$WORK/$1.session"
}

# 1. One file, then two. The injected clicks are at the two inputs' fixed
#    positions (see the page's stylesheet).
script answer <<EOF
load $PAGE
wait 3000
click 160 160
wait 1500
file-dialog-respond 1 $WORK/chosen.txt
wait 1200
click 160 260
wait 1500
file-dialog-respond 2 $WORK/one.txt;$WORK/two.txt
wait 1200
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo answer "$WORK/answer.log"
# CEF does not promise the filters in the attribute's own order, so both
# orders are accepted here; what matters is that both reached the host.
expect 'helper: file-dialog 1 mode=open browser=1 accept=(image/png\+\.txt|\.txt\+image/png)' \
  "$WORK/answer.log" answer
# The page's filters are what the host gets to draw; the demo joins them with
# the same plus its trace uses.
expect 'demo: file-dialog 1 browser=1 mode=open title= default_path=.* accept=(image/png\+\.txt|\.txt\+image/png) policy=ask' \
  "$WORK/answer.log" answer
expect 'session: file-dialog 1 \(open\) .*, host took it: 1' "$WORK/answer.log" answer
expect "demo: file-dialog 1 answer $WORK/chosen\\.txt" "$WORK/answer.log" answer
expect 'helper: file-dialog 1 answered 1 path\(s\)' "$WORK/answer.log" answer
expect_after 'helper: file-dialog 1 mode=open' 'demo: file-dialog 1 answer' \
  "$WORK/answer.log" answer
expect 'helper: file-dialog 2 mode=open-multiple browser=1 accept=none' \
  "$WORK/answer.log" answer
expect "demo: file-dialog 2 answer $WORK/one\\.txt;$WORK/two\\.txt" \
  "$WORK/answer.log" answer
expect 'helper: file-dialog 2 answered 2 path\(s\)' "$WORK/answer.log" answer
expect "demo: script [0-9]+ ok value=\"FILE-DIALOG-PAGE-START\\|single=chosen\\.txt\\|many=one\\.txt,two\\.txt\"" \
  "$WORK/answer.log" answer
expect_no 'cancelled by default' "$WORK/answer.log" answer

# 2. An explicit answer with no paths is the cancel button, and the page hears
#    the input's cancel event rather than a change to an empty selection.
script cancel <<EOF
load $PAGE
wait 3000
click 160 160
wait 1500
file-dialog-respond 1
wait 1200
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo cancel "$WORK/cancel.log"
expect 'demo: file-dialog 1 cancel$' "$WORK/cancel.log" cancel
expect 'helper: file-dialog 1 answered 0 path\(s\)' "$WORK/cancel.log" cancel
expect 'demo: script [0-9]+ ok value="FILE-DIALOG-PAGE-START\|single-cancel"' \
  "$WORK/cancel.log" cancel
expect_no 'demo: file-dialog 1 answer' "$WORK/cancel.log" cancel

# 3. No host chooser: the session cancels, which is the only answer that does
#    not invent a path the page never asked for.
script default <<EOF
load $PAGE
wait 3000
click 160 160
wait 1500
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo default "$WORK/default.log" CEFNEST_DIALOG=default
expect 'demo: file-dialog 1 browser=1 mode=open .* policy=default' "$WORK/default.log" default
expect 'session: file-dialog 1 \(open\) .*, host took it: 0' "$WORK/default.log" default
expect 'session: file-dialog 1 \(open\) cancelled by default' "$WORK/default.log" default
expect 'helper: file-dialog 1 answered 0 path\(s\)' "$WORK/default.log" default
expect 'demo: script [0-9]+ ok value="FILE-DIALOG-PAGE-START\|single-cancel"' \
  "$WORK/default.log" default
# The panel never owned this one, so it never says it answered.
expect_no 'demo: file-dialog 1 (answer|cancel)$' "$WORK/default.log" default

echo
if [ "$fails" -eq 0 ]; then
  printf 'file dialog gate: passed\n'
else
  printf 'file dialog gate: %d check(s) failed\n' "$fails"
  for f in answer cancel default; do
    [ -s "$WORK/$f.log" ] || continue
    echo "-- $f"; tail -50 "$WORK/$f.log" | sed 's/^/  /'
  done
  exit 1
fi
