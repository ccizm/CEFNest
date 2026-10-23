#!/usr/bin/env bash
#
# test/helper-signal-gate.sh — a helper stopped by a signal still shuts CEF down.
#
# `systemctl stop`, a desktop-session logout and a plain `kill` end the helper
# with SIGTERM/SIGHUP/SIGINT. Left at the default action those stop it inside
# CefRunMessageLoop: no CefShutdown, and the profile it owns keeps a
# SingletonLock the next helper on that profile then collides with ("Opening in
# an existing browser session").
#
# No other gate here covers this, because the host's own stop is not a signal
# anyone answers: chromium_session_stop() force-exits the child, i.e. SIGKILL.
# So the gate signals the helper from outside, the way a session manager does,
# and reads back what the helper and the host each reported.
#
# The last two assertions are the pair that matters. A handler that runs but
# leaves the shutdown incomplete is the same bug with better logging, and the
# first version of this gate caught exactly that: the helper announced
# "caught signal 15", released its SingletonLock, and then died inside
# CefShutdown() — because the host, seeing the helper's own {"event":"closed"},
# ended its run and its dispose force-exited the child over that announcement.
# chromium-session.c now leaves a helper that said it is leaving alone, and the
# tail of main() runs to "helper: exited".
#
# Run it inside a nested session so nothing lands on the desktop of whoever is
# sitting here:
#
#   ./test/ci-session.sh ./test/helper-signal-gate.sh
#
# docs/internals/sandbox-policy.md §9 records why standalone mode cannot be used
# for this: under `gnome-shell --headless` its CefInitialize never returns
# (measured on both the current and the previous binary), so the helper has to
# come up the way a real host brings it up.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO=$ROOT/builddir/src/cefnest-demo
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/dnd-page.html"

[ -x "$HELPER" ] || { echo "no helper at $HELPER — cmake --build helper/build"; exit 1; }
[ -x "$DEMO" ] || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
[ -n "${WAYLAND_DISPLAY:-}" ] || {
  echo "  skip: needs a Wayland display — run it as"
  echo "        ./test/ci-session.sh ./test/helper-signal-gate.sh"
  exit 77
}

RUN=$(mktemp -d)
CACHE_BASE=${XDG_CACHE_HOME:-${HOME}/.cache}
fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

demo=""
cleanup() {
  [ -z "$demo" ] || kill -KILL "$demo" 2>/dev/null
  rm -rf "$RUN"
}
trap cleanup EXIT

# One run of the whole battery for one signal: $1 = name, $2 = number the
# transcript has to name.
signal_case() {
  local name=$1 number=$2
  local log=$RUN/$name.log
  local session=$RUN/$name.session

  # Long enough that the signal lands on a helper with a page showing, and the
  # demo is still around to report how the process ended.
  { echo "load $PAGE"; echo "wait 12000"; echo "quit"; } > "$session"

  # The previous case's profiles: SingletonLock is found by name below, so an
  # old one would be read as this case leaving it behind.
  rm -rf "$CACHE_BASE"/cefnest-helper*
  env CHROMIUM_SESSION_SCRIPT="$session" "$DEMO" >"$log" 2>&1 &
  demo=$!

  local helper=""
  for _ in $(seq 1 120); do
    kill -0 "$demo" 2>/dev/null || break
    helper=$(pgrep -P "$demo" -f cefnest-helper 2>/dev/null | head -1)
    [ -n "$helper" ] && break
    sleep 0.25
  done
  if [ -z "$helper" ]; then
    bad "$name: the demo never got a helper to signal"
    tail -10 "$log" | sed 's/^/         /'
    return
  fi

  local loaded=0
  for _ in $(seq 1 160); do
    grep -q "demo: loaded" "$log" && { loaded=1; break; }
    kill -0 "$demo" 2>/dev/null || break
    sleep 0.25
  done
  if [ "$loaded" != 1 ]; then
    bad "$name: the page never loaded, so nothing was running to interrupt"
    tail -10 "$log" | sed 's/^/         /'
    kill -KILL "$demo" 2>/dev/null
    wait "$demo" 2>/dev/null
    demo=""
    return
  fi

  local lock
  lock=$(find "$CACHE_BASE" -name SingletonLock -print -quit 2>/dev/null)
  if [ -z "$lock" ]; then
    bad "$name: no SingletonLock to begin with — the last check would prove nothing"
    return
  fi
  ok "$name: helper $helper up with a page and $lock"

  # The kernel's own answer to "is anyone catching this signal?", read straight
  # out of the process: CefInitialize resets SIGHUP/SIGINT/SIGTERM to SIG_DFL on
  # the way through, so a handler installed before that call is silently gone and
  # the assertions below would only report a missing transcript line.
  local caught want
  caught=$(sed -n 's/^SigCgt:\t\([0-9a-fA-F]*\)$/\1/p' "/proc/$helper/status")
  want=$(( 1 << (number - 1) ))
  if [ $(( 0x$caught & want )) != 0 ]; then
    ok "$name: SIG$name is caught (SigCgt $caught)"
  else
    bad "$name: SIG$name is not caught (SigCgt $caught) — nobody installed a handler, or CEF took it back"
  fi

  kill "-$name" "$helper"

  local gone=0
  for _ in $(seq 1 120); do
    kill -0 "$helper" 2>/dev/null || { gone=1; break; }
    sleep 0.25
  done
  if [ "$gone" != 1 ]; then
    bad "$name: the helper was still alive 30 s after the signal"
    kill -KILL "$helper" 2>/dev/null
  fi

  grep -q "helper: caught signal $number" "$log" \
    && ok "$name: handler ran and named signal $number on the UI thread" \
    || { bad "$name: no 'caught signal $number' in the transcript";
         tail -10 "$log" | sed 's/^/         /'; }

  # Printed after CefShutdown(): the tail of main(), which is the whole point.
  grep -q "helper: exited" "$log" \
    && ok "$name: reached the tail of main() after CefShutdown" \
    || { bad "$name: 'helper: exited' missing — CefShutdown did not run"
         tail -25 "$log" | sed 's/^/         /'; }

  # The host's own read of the wait status: a signal death is what this gate is
  # there to prevent, and the host reports it in its words, not the helper's.
  if grep -q "session: helper killed by signal" "$log"; then
    bad "$name: the host saw a signal death — $(grep -m1 'killed by signal' "$log")"
  else
    ok "$name: the host saw no signal death"
  fi

  [ -e "$lock" ] && bad "$name: $lock left behind — next helper on this profile collides" \
                 || ok "$name: the profile released its SingletonLock"

  kill -KILL "$demo" 2>/dev/null
  wait "$demo" 2>/dev/null
  demo=""
}

echo "== helper signal gate ($HELPER)"
signal_case TERM 15
signal_case HUP 1

if [ "$fails" = 0 ]; then
  echo "== helper signal gate: all green"
  exit 0
fi
echo "== helper signal gate: $fails failed"
exit 1
