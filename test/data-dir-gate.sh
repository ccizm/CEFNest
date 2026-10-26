#!/usr/bin/env bash
#
# test/data-dir-gate.sh — drives the data-directory contract through the demo.
#
# The unit test (test/data-dir.c) proves the path rules and the lock without a
# browser. This one proves the parts only a real helper can: where an ephemeral
# profile is created, that nothing lands in ~/.cache or ~/.config/cef_user_data,
# that a persistent profile survives a restart (localStorage read back), that a
# second user of the same directory fails loudly, and that a killed helper's
# tmpfs root is swept by the next one.
#
#   ./test/data-dir-gate.sh
#   ./test/data-dir-gate.sh default persist relative in-use sweep
#
# HOME and XDG_RUNTIME_DIR are redirected into a sandbox for every demo run, so
# nothing under the real home is touched. The outer Wayland display is made
# absolute first: XDG_RUNTIME_DIR is where its socket lives, and the sandbox has
# its own.
#
# Runs inside test/ci-session.sh like the other real-browser gates; a machine
# without a Wayland display skips with status 77.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/data-dir-page.html"

WHICH=${1:-all}
want() { # want <name>
  [ "$WHICH" = all ] || [ "$WHICH" = "$1" ]
}

WORK=$(mktemp -d)
DEMO_PID=
fails=0

cleanup() {
  if [ -n "$DEMO_PID" ]; then
    kill -TERM "$DEMO_PID" 2>/dev/null
    wait "$DEMO_PID" 2>/dev/null
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qaE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    tail -8 "$2" 2>/dev/null | sed 's/^/         /'
  fi
}

expect_no_file() { # expect_no_file <path> <tag>
  if [ -e "$1" ]; then bad "$2: $1 exists"; else ok "$2: $1 absent"; fi
}

[ -x "$DEMO" ] || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
[ -x "$HELPER" ] || { echo "no helper at $HELPER — run cmake --build helper/build"; exit 1; }

# The display this gate draws on. Its socket is resolved against the current
# XDG_RUNTIME_DIR, which is about to be replaced by the sandbox's.
case "${WAYLAND_DISPLAY:-}" in
  /*) OUTER_WAYLAND=$WAYLAND_DISPLAY ;;
  ?*) OUTER_WAYLAND=${XDG_RUNTIME_DIR:-}/$WAYLAND_DISPLAY ;;
  *) OUTER_WAYLAND= ;;
esac
if [ -z "$OUTER_WAYLAND" ] || [ "${OUTER_WAYLAND#/}" = "$OUTER_WAYLAND" ]; then
  echo "no absolute Wayland display to run against"
  exit 77
fi
if [ ! -S "$OUTER_WAYLAND" ]; then
  echo "no Wayland socket at $OUTER_WAYLAND"
  exit 77
fi

SANDBOX=$WORK/sandbox
HOME_DIR=$SANDBOX/home
RT=$SANDBOX/run
mkdir -p "$HOME_DIR/.cache" "$HOME_DIR/.config" "$HOME_DIR/.local/share" "$RT"
chmod 700 "$RT"

demo_env=(
  "HOME=$HOME_DIR"
  "XDG_RUNTIME_DIR=$RT"
  "XDG_CONFIG_HOME=$HOME_DIR/.config"
  "XDG_CACHE_HOME=$HOME_DIR/.cache"
  "XDG_DATA_HOME=$HOME_DIR/.local/share"
  "WAYLAND_DISPLAY=$OUTER_WAYLAND"
  "GDK_BACKEND=wayland"
  "CHROMIUM_HELPER=$HELPER"
  # A value the host inherited must never decide the helper's mode: the library
  # unsets it before spawning unless the host called set_data_directory().
  "CHROMIUM_HELPER_DATA_DIR=$WORK/inherited"
)

roots() { find "$RT" -maxdepth 1 -type d -name 'cefnest-helper-*' 2>/dev/null; }
has_root() { [ -n "$(roots)" ]; }

wait_for() { # wait_for <seconds> <command...>
  local deadline=$(( $(date +%s) + $1 )); shift
  while [ "$(date +%s)" -lt "$deadline" ]; do
    "$@" && return 0
    sleep 0.2
  done
  return 1
}

script() { # script <tag> <lines...>
  local file=$WORK/$1.session; shift
  printf '%s\n' "$@" > "$file"
  printf '%s' "$file"
}

start_demo() { # start_demo <log> <script-file> [args...]
  local log=$1 script=$2; shift 2
  env "${demo_env[@]}" CHROMIUM_SESSION_SCRIPT="$script" \
    "$DEMO" "$@" >"$log" 2>&1 &
  DEMO_PID=$!
}

# A demo with no `quit` line still has to end the gate rather than hang it:
# give it <timeout> seconds, then stop asking nicely.
stop_demo() { # stop_demo [timeout-seconds]
  local tmo=${1:-90} status
  local deadline=$(( $(date +%s) + tmo ))
  while kill -0 "$DEMO_PID" 2>/dev/null &&
        [ "$(date +%s)" -lt "$deadline" ]; do
    sleep 0.2
  done
  if kill -0 "$DEMO_PID" 2>/dev/null; then
    kill -TERM "$DEMO_PID" 2>/dev/null
    sleep 1
    kill -KILL "$DEMO_PID" 2>/dev/null
  fi
  wait "$DEMO_PID" 2>/dev/null
  status=$?
  DEMO_PID=
  return $status
}

# ---------------------------------------------------------------------------
# 1. Default: an ephemeral root on tmpfs, nothing under HOME, gone on exit.
# ---------------------------------------------------------------------------
if want default; then
  echo "== default: ephemeral root, no trace under HOME"
  S=$(script default "load $PAGE" "wait 6000" "quit")
  start_demo "$WORK/default.log" "$S"
  if wait_for 25 has_root; then
    ok "default: an ephemeral root exists while the helper runs"
  else
    bad "default: no cefnest-helper-* directory in \$XDG_RUNTIME_DIR"
  fi

  root=$(roots | head -1)
  if [ -n "$root" ]; then
    mode=$(stat -c %a "$root")
    if [ "$mode" = 700 ]; then ok "default: the root is mode 0700"
    else bad "default: the root is mode $mode, wanted 0700"; fi
    if [ -d "$root/browser" ]; then
      bad "default: root/browser exists — the profile is not in memory"
    else
      ok "default: there is no on-disk browser profile"
    fi
  fi

  if find "$HOME_DIR/.cache" -maxdepth 1 -name 'cefnest-helper*' | grep -q .; then
    bad "default: ~/.cache gained a cefnest-helper entry"
  else
    ok "default: ~/.cache holds no cefnest-helper entry"
  fi
  expect_no_file "$HOME_DIR/.config/cef_user_data" "default: no CEF fallback profile"
  expect_no_file "$WORK/inherited" "default: the inherited data directory stayed unused"

  if stop_demo; then ok "default: demo exited 0"
  else bad "default: demo exited nonzero"; fi

  expect 'cache mode=ephemeral root=' "$WORK/default.log" "default: helper named its mode"
  if [ -n "$root" ] && [ -e "$root" ]; then
    bad "default: the ephemeral root survived the exit"
    pid=${root##*/cefnest-helper-}
    pid=${pid%%-*}
    if kill -0 "$pid" 2>/dev/null; then
      echo "         helper $pid is still running"
    else
      echo "         helper $pid is gone; the root outlived it"
    fi
    find "$root" -maxdepth 1 | head -5 | sed 's/^/         /'
    grep -aE 'session:|helper:' "$WORK/default.log" | tail -15 | sed 's/^/         /'
  else
    ok "default: the ephemeral root is gone after exit"
  fi
fi

# ---------------------------------------------------------------------------
# 2. Persistent: one directory, two runs, localStorage read back in the second.
# ---------------------------------------------------------------------------
if want persist; then
  echo "== persist: localStorage survives a restart"
  P=$WORK/persistent
  S=$(script persist "load $PAGE" "wait 5000" \
        "js localStorage.getItem('cefnest-gate')" "wait 500" "quit")

  start_demo "$WORK/persist1.log" "$S" --data-dir="$P"
  if stop_demo; then ok "persist: the first run exited 0"
  else bad "persist: the first run exited nonzero"; fi
  expect "cache mode=persistent root=$P profile=$P/browser" \
    "$WORK/persist1.log" "persist: helper named the profile"
  if [ -d "$P/browser" ]; then ok "persist: $P/browser exists after the run"
  else bad "persist: $P/browser was not created"; fi
  if [ -e "$P/.cefnest.lock" ]; then ok "persist: the helper's lock file is there"
  else bad "persist: the lock file is missing"; fi

  start_demo "$WORK/persist2.log" "$S" --data-dir="$P"
  if stop_demo; then ok "persist: the second run exited 0"
  else bad "persist: the second run exited nonzero"; fi
  expect 'script [0-9]+ ok value="persisted"' "$WORK/persist2.log" \
    "persist: the page read its own value back"
  if [ -d "$P/browser" ]; then ok "persist: the profile is still there"
  else bad "persist: the profile disappeared"; fi
fi

# ---------------------------------------------------------------------------
# 3. A relative path is refused by start(), and the demo says so and exits.
# ---------------------------------------------------------------------------
if want relative; then
  echo "== relative: an absolute path is required"
  env "${demo_env[@]}" timeout --kill-after=10s 30 "$DEMO" --data-dir=relative/profile \
    >"$WORK/relative.log" 2>&1
  status=$?
  if [ "$status" = 1 ]; then ok "relative: the demo exited 1"
  else bad "relative: the demo exited $status, wanted 1"; fi
  expect 'must be an absolute path' "$WORK/relative.log" "relative: the error names the rule"
fi

# ---------------------------------------------------------------------------
# 4. Two users of one directory: the host precheck refuses the second, and the
#    helper's own lock refuses even when the precheck is bypassed.
# ---------------------------------------------------------------------------
if want in-use; then
  echo "== in-use: one directory, one helper"
  P=$WORK/in-use
  S=$(script in-use "load $PAGE" "wait 15000" "quit")

  start_demo "$WORK/in-use1.log" "$S" --data-dir="$P"
  if wait_for 25 test -d "$P/browser"; then
    ok "in-use: the first helper owns the directory"
  else
    bad "in-use: the first helper never created $P/browser"
  fi

  env "${demo_env[@]}" timeout --kill-after=10s 30 "$DEMO" --data-dir="$P" \
    >"$WORK/in-use2.log" 2>&1
  status=$?
  if [ "$status" = 1 ]; then ok "in-use: the second host exited 1"
  else bad "in-use: the second host exited $status, wanted 1"; fi
  expect 'data directory is in use by another session' "$WORK/in-use2.log" \
    "in-use: the host precheck named the collision"

  # The helper is the authority: run it directly, with no host precheck in the
  # way, and it has to refuse on its own flock.
  env "${demo_env[@]}" CHROMIUM_HELPER_DATA_DIR="$P" \
    timeout --kill-after=10s 20 "$HELPER" >"$WORK/in-use3.log" 2>&1
  status=$?
  if [ "$status" = 1 ]; then ok "in-use: the helper exited 1"
  else bad "in-use: the helper exited $status, wanted 1"; fi
  expect "helper: data directory is in use: $P" "$WORK/in-use3.log" \
    "in-use: the helper's own lock refused it"

  kill -TERM "$DEMO_PID" 2>/dev/null
  wait "$DEMO_PID" 2>/dev/null
  DEMO_PID=
  if [ -d "$P/browser" ]; then ok "in-use: the first profile survived the stop"
  else bad "in-use: the first profile was removed"; fi
fi

# ---------------------------------------------------------------------------
# 5. A killed helper leaves its root; the next helper's sweep removes it.
# ---------------------------------------------------------------------------
if want sweep; then
  echo "== sweep: a killed helper's tmpfs root does not pile up"
  S=$(script sweep "load $PAGE" "wait 15000" "quit")

  start_demo "$WORK/sweep1.log" "$S"
  if wait_for 25 has_root; then
    ok "sweep: the victim's root exists"
  else
    bad "sweep: no root to kill"
  fi

  victim=$(roots | head -1)
  pid=${victim##*/cefnest-helper-}
  pid=${pid%%-*}
  kill -9 "$pid" 2>/dev/null
  stop_demo 30 >/dev/null 2>&1 || true
  if [ -d "$victim" ]; then ok "sweep: the killed helper left its root behind"
  else bad "sweep: the root vanished without a sweep"; fi

  S2=$(script sweep-again "load $PAGE" "wait 2000" "quit")
  start_demo "$WORK/sweep2.log" "$S2"
  if stop_demo; then ok "sweep: the next helper ran and exited 0"
  else bad "sweep: the next helper exited nonzero"; fi
  if [ -e "$victim" ]; then bad "sweep: the dead root is still there"
  else ok "sweep: the next helper removed the dead root"; fi
fi

echo
if [ $fails = 0 ]; then
  printf 'data-dir gate: passed\n'
else
  printf 'data-dir gate: %d check(s) failed\n' "$fails"
  exit 1
fi
