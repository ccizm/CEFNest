#!/usr/bin/env bash
#
# test/sandbox-soak.sh — drives the sandboxed helper long enough to see what its
# process tree does, and asserts on the shape of it.
#
# One demo window, twenty navigations over four local pages, two hard reloads, and
# a deliberate `kill -9` of every renderer at two points inside the run. What gets
# asserted is the tree and not the pixels: it keeps its size, nothing becomes a
# zombie, the killed renderers come back, the page keeps answering through both
# kills, no crash handler is ever attached to a child of ours, and nothing is left
# running once the demo has gone. docs/sandbox-policy.md §7 and §8 are the numbers
# this gate keeps from going stale.
#
# It opens a real window for about a minute, so it is a documented manual gate —
# the same convention as test/find-gate.sh and test/dnd-gate.sh. Nothing here
# moves a pointer on the host desktop.
#
#   ./test/sandbox-soak.sh
#
# FD_SLACK is the drift the fd count is allowed over the run; 40 covered 20
# navigations and 7 hard reloads with room to spare (measured: 14).

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
HELPER="$ROOT/helper/build/Release/cefnest-helper"
PAGES=(dnd-page.html find-page.html wheel-page.html menu-page.html)
ROUNDS=20
FD_SLACK=${FD_SLACK:-40}
KILL_AT=(12 28)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

[ -x "$DEMO" ]   || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
[ -x "$HELPER" ] || { echo "no helper at $HELPER — run cmake --build helper/build"; exit 1; }

{
  for i in $(seq 1 $ROUNDS); do
    echo "load file://$ROOT/test/${PAGES[$(( (i - 1) % ${#PAGES[@]} ))]}"
    echo "wait 1400"
    echo "js document.title"
    echo "wait 250"
    if (( i % 7 == 3 )); then echo "reload-hard"; echo "wait 1400"; fi
  done
  echo "wait 3000"
  echo "quit"
} > "$WORK/soak.session"

# Every process running our helper binary, at any depth. Walking from the browser
# process alone does not work: the renderers are children of the zygote, so a
# monitor that counts only direct children of the browser never sees one die.
helper_pids() { pgrep -f "$HELPER"; }

browser_pid() {
  local pid
  for pid in $(helper_pids); do
    if ! tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- '--type='; then
      echo "$pid"; return
    fi
  done
}

# One line per sample: elapsed pids typed renderers zombies fds rss_kB crashpad_attached
sample() {
  local pid cmd state fds rss
  local pids n=0 typed=0 render=0 zomb=0 fdsum=0 rsssum=0 attached=0
  pids=$(helper_pids | tr '\n' ' ')
  [ -n "${pids// /}" ] || return 1
  for pid in $pids; do
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
    state=$(awk '/^State:/{print $2}' "/proc/$pid/status" 2>/dev/null)
    [ "$state" = "Z" ] && zomb=$(( zomb + 1 ))
    case "$cmd" in *--type=*) typed=$(( typed + 1 )); esac
    case "$cmd" in *"--type=renderer"*) render=$(( render + 1 )); esac
    # The visible sign of a crash handler is this on a child's command line.
    case "$cmd" in *--crashpad-handler-pid=*) attached=$(( attached + 1 )); esac
    fdsum=$(( fdsum + $(ls "/proc/$pid/fd" 2>/dev/null | wc -l) ))
    rsssum=$(( rsssum + $(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0) ))
  done
  n=$(echo $pids | wc -w)
  printf '%s %s %s %s %s %s %s %s\n' "$1" "$n" "$typed" "$render" "$zomb" "$fdsum" "$rsssum" "$attached"
}

kill_renderers() { # kill_renderers <tag> — returns the count killed, records the parents
  local pid ppid browser killed=0
  browser=$(browser_pid)
  for pid in $(helper_pids); do
    if tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- '--type=renderer'; then
      ppid=$(awk '/^PPid:/{print $2}' "/proc/$pid/status" 2>/dev/null)
      echo "$1 $pid $ppid $browser" >> "$WORK/parents.txt"
      kill -9 "$pid" 2>/dev/null && killed=$(( killed + 1 ))
    fi
  done
  echo "$1 $killed" >> "$WORK/kills.txt"
}

GDK_BACKEND=wayland CHROMIUM_HELPER="$HELPER" \
CHROMIUM_SESSION_SCRIPT="$WORK/soak.session" \
  "$DEMO" >"$WORK/soak.out" 2>"$WORK/soak.log" &
DEMO_PID=$!

START=$(date +%s)
: > "$WORK/samples.txt"
next_kill=0
while kill -0 "$DEMO_PID" 2>/dev/null; do
  t=$(( $(date +%s) - START ))
  sample "$t" >> "$WORK/samples.txt" || true
  if [ "$next_kill" -lt "${#KILL_AT[@]}" ] && [ "$t" -ge "${KILL_AT[$next_kill]}" ]; then
    kill_renderers "kill$(( next_kill + 1 ))"
    next_kill=$(( next_kill + 1 ))
  fi
  sleep 2
done

echo "== soak"

# 1. The run has to have sampled a tree, and had something to kill.
if [ "$(wc -l < "$WORK/samples.txt")" -ge 8 ]; then
  ok "soak: $(wc -l < "$WORK/samples.txt") samples over ${t}s"
else bad "soak: only $(wc -l < "$WORK/samples.txt") samples — nothing was measured"; fi
if [ "$(grep -c . "$WORK/parents.txt" 2>/dev/null || echo 0)" -ge 2 ]; then
  ok "soak: renderers were found and killed at both points"
else bad "soak: fewer than two renderer kills — the tree did not look like the documented one"; fi

# 2. Nothing went zombie, and no crash handler ever appeared.
if [ "$(awk '{s+=$5} END{print s+0}' "$WORK/samples.txt")" = 0 ]; then
  ok "soak: zero zombie children across the whole run"
else bad "soak: $(awk '{s+=$5} END{print s}' "$WORK/samples.txt") zombie sample(s) — a reaped child leaked"; fi
if [ "$(awk '{s+=$8} END{print s+0}' "$WORK/samples.txt")" = 0 ]; then
  ok "soak: no child ever carried --crashpad-handler-pid (docs/sandbox-policy.md §8)"
else bad "soak: a crash handler is attached now — §8 measured none, so its verdict needs re-running"; fi

# 3. The tree has the shape the walk assumes: a renderer's parent is not the
#    browser process, it is the zygote.
if awk '$3 == $4 {print}' "$WORK/parents.txt" | grep -q .; then
  bad "soak: a renderer was a direct child of the browser — the tree changed shape"
else ok "soak: every renderer was a child of the zygote, not of the browser"; fi

# 4. Baseline held, taken from the first sample after the tree had settled — the
#    one the demo starts with sees no renderer yet, and a four-second window after
#    each kill is the shortest one a zygote replacement reliably fits in.
base=$(awk '$1 >= 16 && !seen { print $4; seen = 1 }' "$WORK/samples.txt")
basep=$(awk '$1 >= 16 && !seen { print $2; seen = 1 }' "$WORK/samples.txt")
if [ -z "$base" ] || [ "$base" = 0 ]; then
  bad "soak: the settled tree had $base renderer(s) — no page process to watch"
else
  moved=$(awk -v b="$base" -v p="$basep" '$1 >= 16 && ($2 != p || $4 != b)' "$WORK/samples.txt")
  if [ -z "$moved" ]; then
    ok "soak: $base renderer(s) and $basep processes in every sample from t=16 on"
  else
    bad "soak: tree moved after a kill — $(echo "$moved" | head -3 | tr '\n' ';')"
  fi
fi

# 5. fd count did not run away, measured across the settled window — a sample
#    taken before the tree is whole counts fewer descriptors than it leaks. RSS is
#    printed, not pinned: it is a sawtooth and machine-dependent
#    (docs/sandbox-policy.md §7 has the measured curve).
base_fd=$(awk '$1 >= 16 && !seen { print $6; seen = 1 }' "$WORK/samples.txt")
last_fd=$(awk 'END{print $6}' "$WORK/samples.txt")
peak_fd=$(awk '$1 >= 16 && $6 > m { m = $6 } END { print m+0 }' "$WORK/samples.txt")
last_rss=$(awk 'END{print int($7/1024)}' "$WORK/samples.txt")
if [ -z "$base_fd" ]; then
  bad "soak: no sample at or after t=16 — the run was too short to measure drift"
elif [ $(( last_fd - base_fd )) -le "$FD_SLACK" ] && [ $(( peak_fd - base_fd )) -le "$FD_SLACK" ]; then
  ok "soak: fd count $base_fd → $last_fd, peak $peak_fd over ${t}s and $ROUNDS navigations (slack $FD_SLACK)"
else bad "soak: fd count went $base_fd → $last_fd, peak $peak_fd, more than the $FD_SLACK slack"; fi
ok "soak: tree RSS at the last sample was ${last_rss} MB (reported, not asserted)"

# 6. The page answered through both kills. A readback can legitimately be lost in
#    the millisecond a renderer dies in, so this is a floor, not an equality.
answered=$(grep -cE 'script [0-9]+ ok' "$WORK/soak.log")
if [ "$answered" -ge $(( ROUNDS - 2 )) ]; then
  ok "soak: $answered of $ROUNDS JavaScript readbacks answered through two renderer kills"
else bad "soak: only $answered of $ROUNDS readbacks answered — the tree did not recover"; fi

# 7. Two kills, exactly two events, and the ones the doc says.
if grep -cE 'renderer exited' "$WORK/soak.log" | grep -qx 2; then
  ok "soak: two renderer deaths reported, one line each"
else
  bad "soak: $(grep -cE 'renderer exited' "$WORK/soak.log") renderer-exited line(s), wanted 2"
fi
if grep -qE 'renderer exited status=1 error=9' "$WORK/soak.log"; then
  ok "soak: reported as STATUS_CRASHED with signal 9 — the OS-level death, and all there is"
else bad "soak: no 'status=1 error=9' line — a kill -9 is reported differently now"; fi

# 8. Nothing outlived the demo.
left=$(helper_pids | grep -c . || true)
if [ "$left" = 0 ]; then ok "soak: no helper process left after the demo exited"
else bad "soak: $left helper process(es) survived the run"; fi

echo
if [ $fails -eq 0 ]; then
  printf 'sandbox soak: all checks passed\n'
else
  printf 'sandbox soak: %d check(s) failed\n' "$fails"
  echo "samples: $(cat "$WORK/samples.txt" | head -3 | tr '\n' ';') … log: $WORK/soak.log"
  exit 1
fi
