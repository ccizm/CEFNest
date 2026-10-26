#!/usr/bin/env bash
#
# test/js-dialog-gate.sh — the page's own dialogs, end to end.
#
# Four scripted runs against the real CEF helper and the demo:
#
#   1. ask          alert on load, then confirm and prompt asked from script;
#                   each is answered through the panel's script step and the
#                   page reads its own results back
#   2. default      the host declines the signal; the session answers with
#                   Chrome's defaults and the page sees cancel/null
#   3. beforeunload an armed page asks on reload, is told to stay, asks again
#                   and is allowed to leave
#   4. close        the browser closes under a parked alert: the helper drops
#                   the request and the host hears ::js-dialog-closed before
#                   ::browser-closed
#
# The measurement behind this gate: with no handler the load-time alert parked
# the page forever and the host saw nothing at all. Answering is the only way a
# page with alert() in it makes progress, so every run here asserts both sides
# of that answer — the page's own transcript and the helper's "answered" line.
#
# The beforeunload run needs sticky activation, which the injected click gives
# the page; Chromium refuses to show the dialog otherwise. All input goes into
# the demo's nested compositor, never at the desktop pointer.
#
#   ./test/js-dialog-gate.sh
#   ./test/ci-session.sh ./test/js-dialog-gate.sh
#   KEEP_LOGS=1 ./test/js-dialog-gate.sh
#
# Every run pipes both streams through tee for the reason permission-gate.sh
# documents: the session's debug lines are half the contract and GLib sends
# them to stdout once the process is off a tty.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PAGE="file://$ROOT/test/js-dialog-page.html"
UNLOAD_PAGE="file://$ROOT/test/beforeunload-page.html"
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

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'js-dialog|script [0-9]+' "$2" | head -10 | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_count() { # expect_count <pattern> <logfile> <count> <tag>
  local n
  n=$(grep -cE "$1" "$2")
  if [ "$n" = "$3" ]; then ok "$4: $n line(s) match /$1/"
  else bad "$4: $n line(s) match /$1/, wanted $3"; fi
}

# The order is the contract in two places here: a page load cannot finish while
# its alert is parked, and a browser cannot report itself closed before the
# dialog it dropped has been reported. Two greps cannot say that.
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

# What the page itself saw. The js step reports its value quoted, one line per
# step, so the n-th one is the n-th read-back.
js_value() { # js_value <logfile> <n>
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

run_demo() { # run_demo <tag> <logfile> <wanted rc> <extra env...>
  local tag=$1 log=$2 want=$3
  shift 3

  if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
  if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
  echo "== $tag"
  timeout --kill-after=10s 90 env "$@" G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/$tag.session" \
    "$DEMO" 2>&1 | tee "$log" | cat > /dev/null
  local status=${PIPESTATUS[0]}
  if [ "$status" = "$want" ]; then ok "$tag: demo exited $status"
  else
    bad "$tag: demo exited $status, $want wanted"
    tail -8 "$log" | sed 's/^/         /'
  fi
}

script() { # script <tag> — writes the heredoc that follows into the tag's script
  cat > "$WORK/$1.session"
}

# 1. All three kinds answered by the host, with a prompt value that has to
#    survive the whole trip: page -> helper -> host -> helper -> page.
script ask <<EOF
load $PAGE
wait 2500
js-dialog-respond 1 ok
wait 600
js window.askConfirm()
wait 600
js-dialog-respond 2 cancel
wait 600
js window.askPrompt()
wait 600
js-dialog-respond 3 ok hello world
wait 600
js-dialog-respond 3 cancel again
wait 400
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo ask "$WORK/ask.log" 0
expect 'demo: dialogs: policy=ask' "$WORK/ask.log" ask
expect 'helper: js-dialog 1 kind=alert browser=1 origin=file:///.*js-dialog-page\.html' \
  "$WORK/ask.log" ask
expect 'demo: js-dialog 1 browser=1 kind=alert origin=file:///.* message=page alert default_value= policy=ask' \
  "$WORK/ask.log" ask
expect 'session: js-dialog 1 \(alert\) from .*, host took it: 1' "$WORK/ask.log" ask
expect 'demo: js-dialog 1 ok value=$' "$WORK/ask.log" ask
expect 'helper: js-dialog 1 answered ok=1 value_bytes=0' "$WORK/ask.log" ask
# The page is stopped at the alert: its load cannot have finished before the
# answer, so the first "loaded" line has to come after it.
expect_after 'demo: js-dialog 1 ok' 'demo: loaded .*js-dialog-page\.html \(HTTP 200\)' \
  "$WORK/ask.log" ask
expect 'helper: js-dialog 2 kind=confirm browser=1' "$WORK/ask.log" ask
expect 'demo: js-dialog 2 browser=1 kind=confirm .* message=page confirm default_value= policy=ask' \
  "$WORK/ask.log" ask
expect 'demo: js-dialog 2 cancel value=$' "$WORK/ask.log" ask
expect 'helper: js-dialog 2 answered ok=0 value_bytes=0' "$WORK/ask.log" ask
expect 'helper: js-dialog 3 kind=prompt browser=1' "$WORK/ask.log" ask
expect 'demo: js-dialog 3 browser=1 kind=prompt .* message=page prompt default_value=seed policy=ask' \
  "$WORK/ask.log" ask
expect 'demo: js-dialog 3 ok value=hello world' "$WORK/ask.log" ask
expect 'helper: js-dialog 3 answered ok=1 value_bytes=11' "$WORK/ask.log" ask
# A row this side already decided crosses nothing, and says so.
expect 'demo: no js dialog 3 is waiting for an answer' "$WORK/ask.log" ask
# alert returned undefined, confirm false, prompt the answered text: the page's
# own record of the three answers, not the trace's.
expect 'demo: script [0-9]+ ok value="DIALOG-PAGE-START\|alert=undefined\|ready\|confirm=false\|prompt=hello world"' \
  "$WORK/ask.log" ask
# The host took every question, so the default path never ran.
expect_no 'answered (ok|cancel) by default' "$WORK/ask.log" ask
expect_no 'host took it: 0' "$WORK/ask.log" ask

# 2. No host dialog UI: the session answers, and the page sees Chrome's answers
#    rather than a page that never moves again.
script default <<EOF
load $PAGE
wait 2500
js window.askConfirm()
wait 600
js window.askPrompt()
wait 600
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo default "$WORK/default.log" 0 CEFNEST_DIALOG=default
expect 'demo: dialogs: policy=default' "$WORK/default.log" default
expect 'session: js-dialog 1 \(alert\) from .*, host took it: 0' "$WORK/default.log" default
expect 'session: js-dialog 1 \(alert\) answered ok by default' "$WORK/default.log" default
expect 'helper: js-dialog 1 answered ok=1 value_bytes=0' "$WORK/default.log" default
expect 'session: js-dialog 2 \(confirm\) answered cancel by default' "$WORK/default.log" default
expect 'session: js-dialog 3 \(prompt\) answered cancel by default' "$WORK/default.log" default
expect 'helper: js-dialog 3 answered ok=0 value_bytes=0' "$WORK/default.log" default
# The prompt's default text reached the host even though the host drew nothing.
expect 'demo: js-dialog 3 browser=1 kind=prompt .* default_value=seed policy=default' \
  "$WORK/default.log" default
expect 'demo: script [0-9]+ ok value="DIALOG-PAGE-START\|alert=undefined\|ready\|confirm=false\|prompt=null"' \
  "$WORK/default.log" default
# No button was ever pressed, so the panel never claims to have answered.
expect_no 'demo: js-dialog [0-9]+ (ok|cancel) value=' "$WORK/default.log" default

# 3. beforeunload: the page is armed by a real click, reload asks the question,
#    "stay" keeps the page and its state, "leave" reloads it.
script beforeunload <<EOF
load $UNLOAD_PAGE
wait 2500
click 160 160
wait 800
reload
wait 1500
js-dialog-respond 1 cancel
wait 1500
js document.getElementById("out").textContent
wait 500
reload
wait 1500
js-dialog-respond 2 ok
wait 2500
js document.getElementById("out").textContent
wait 800
close
EOF
run_demo beforeunload "$WORK/beforeunload.log" 0
expect 'helper: js-dialog 1 kind=beforeunload browser=1 reload=1' "$WORK/beforeunload.log" unload
expect 'demo: js-dialog 1 browser=1 kind=beforeunload origin= message=Is it OK to leave/reload this page\? default_value= policy=ask' \
  "$WORK/beforeunload.log" unload
expect 'demo: js-dialog 1 cancel value=$' "$WORK/beforeunload.log" unload
expect 'helper: js-dialog 1 answered ok=0 value_bytes=0' "$WORK/beforeunload.log" unload
# Staying kept the very DOM the page had: its arm flag and its note survived.
[ "$(js_value "$WORK/beforeunload.log" 1)" = "BEFOREUNLOAD-PAGE-START|armed" ] \
  && ok "unload: the page stayed with its state" \
  || bad "unload: after cancel the page reported '$(js_value "$WORK/beforeunload.log" 1)'"
expect 'helper: js-dialog 2 kind=beforeunload browser=1 reload=1' "$WORK/beforeunload.log" unload
expect 'demo: js-dialog 2 ok value=$' "$WORK/beforeunload.log" unload
expect 'helper: js-dialog 2 answered ok=1 value_bytes=0' "$WORK/beforeunload.log" unload
# Leaving reloaded a fresh document, which is why the arm flag is gone again.
[ "$(js_value "$WORK/beforeunload.log" 2)" = "BEFOREUNLOAD-PAGE-START" ] \
  && ok "unload: leaving reloaded the page" \
  || bad "unload: after leave the page reported '$(js_value "$WORK/beforeunload.log" 2)'"
expect_count 'demo: loaded .*beforeunload-page\.html \(HTTP 200\)' "$WORK/beforeunload.log" 2 unload

# 4. The browser closing under a parked dialog. The helper drops the callback
#    without answering, emits js-dialog-closed, and only then browser-closed.
script close <<EOF
load $PAGE
wait 2500
close-browser 1
wait 2500
quit
EOF
run_demo close "$WORK/close.log" 1
expect 'helper: js-dialog 1 kind=alert browser=1' "$WORK/close.log" close
expect 'session: js-dialog 1 \(alert\) from .*, host took it: 1' "$WORK/close.log" close
expect 'helper: browser 1 closing' "$WORK/close.log" close
expect 'helper: js-dialog 1 closed \(browser closed\)' "$WORK/close.log" close
expect 'session: js-dialog 1 closed by the browser' "$WORK/close.log" close
expect 'demo: js-dialog 1 closed' "$WORK/close.log" close
expect 'demo: browser 1 closed' "$WORK/close.log" close
expect_after 'demo: js-dialog 1 closed' 'demo: browser 1 closed' "$WORK/close.log" close
# Dropped is not answered: no Continue() may cross a request CEF already took.
expect_no 'helper: js-dialog 1 answered' "$WORK/close.log" close
expect_no 'answered (ok|cancel) by default' "$WORK/close.log" close
expect 'demo: the helper went away \(helper closed the browser\) with [1-9][0-9]* of' \
  "$WORK/close.log" close

echo
if [ "$fails" -eq 0 ]; then
  printf 'js dialog gate: passed\n'
else
  printf 'js dialog gate: %d check(s) failed\n' "$fails"
  for f in ask default beforeunload close; do
    [ -s "$WORK/$f.log" ] || continue
    echo "-- $f"; tail -50 "$WORK/$f.log" | sed 's/^/  /'
  done
  exit 1
fi
