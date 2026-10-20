#!/usr/bin/env bash
#
# test/ci-session.sh — runs a gate against a Wayland display nobody can see.
#
# Every other gate here opens a window on the host's desktop, which is why they
# are documented manual gates: someone is sitting at the machine, and the run
# moves real windows in front of them. This one starts a nested compositor —
# GNOME Shell 48 in `--headless` mode, which is mutter with no output device —
# and runs its payload against that instead. Nothing appears on the screen, so a
# run can be left unattended, which is what a release gate needs. No package has
# to be installed: gnome-shell, dbus-run-session and /dev/dri are already here.
#
#   ./test/ci-session.sh                        # proof: a window maps, a page loads
#   ./test/ci-session.sh ./test/find-gate.sh real
#   ./test/ci-session.sh meson test -C builddir --suite unit
#
# The nested session gets its own HOME, XDG_*, runtime directory and D-Bus bus.
# That is not tidiness: GNOME Shell started against the real one activated
# nautilus and the Tracker file miner and auto-scaled the app grid, so an
# unattended run would have touched the desktop of whoever is sitting here.
# With an isolated HOME those services either fail quietly or index an empty
# tree. The compositor is a session leader of its own, so the whole tree — the
# bus, the portal, gvfsd — dies with one signal to its process group.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
HELPER=${CHROMIUM_HELPER:-"$ROOT/helper/build/Release/cefnest-helper"}
PAGE="file://$ROOT/test/dnd-page.html"
NAME=${CI_WAYLAND_DISPLAY:-cefnest-ci}
PAYLOAD_TIMEOUT=${PAYLOAD_TIMEOUT:-900}

RUN=$(mktemp -d)
CHOME=$RUN/home
RT=$RUN/xdg
mkdir -p "$CHOME" "$RT"
chmod 700 "$RT"

fails=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

GS=""
stop_compositor() {
  [ -n "$GS" ] || return 0
  kill -TERM -"$GS" 2>/dev/null
  for _ in $(seq 1 60); do
    pgrep -g "$GS" >/dev/null 2>&1 || return 0
    sleep 0.25
  done
  kill -KILL -"$GS" 2>/dev/null
  sleep 1
}

cleanup() {
  stop_compositor
  chmod -R u+rwX "$RUN" 2>/dev/null
  if ! rm -rf "$RUN" 2>/dev/null; then
    echo "  note: the nested session left files behind in $RUN"
  fi
}
trap cleanup EXIT

[ -x "$DEMO" ] || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
command -v gnome-shell >/dev/null || { echo "no gnome-shell on this machine"; exit 1; }
command -v dbus-run-session >/dev/null || { echo "no dbus-run-session"; exit 1; }

# The compositor's own environment: a home nobody else reads, a bus nobody else
# sees, and no inherited WAYLAND_DISPLAY — it must create its own.
gs_env=(
  "HOME=$CHOME"
  "XDG_RUNTIME_DIR=$RT"
  "XDG_CONFIG_HOME=$CHOME/.config"
  "XDG_CACHE_HOME=$CHOME/.cache"
  "XDG_DATA_HOME=$CHOME/.local/share"
  "XDG_STATE_HOME=$CHOME/.local/state"
)

echo "== starting nested mutter (headless)"
# setsid here has no --write-pid, so the session leader writes its own pid: that
# is the process group the whole nested tree — bus, portal, gvfsd — belongs to,
# and the only handle needed to kill it. `env` execs in place, so the pid the
# script prints is the pid setsid's child got.
cat > "$RUN/gs.sh" <<EOF
#!/usr/bin/env bash
printf %s \$\$ > "$RUN/gs.pid"
exec dbus-run-session -- gnome-shell --headless --wayland-display="$NAME"
EOF
chmod +x "$RUN/gs.sh"
setsid --fork env -u WAYLAND_DISPLAY -u DISPLAY "${gs_env[@]}" "$RUN/gs.sh" \
  >"$RUN/gs.log" 2>&1 < /dev/null

for _ in $(seq 1 40); do [ -s "$RUN/gs.pid" ] && break; sleep 0.1; done
GS=$(cat "$RUN/gs.pid" 2>/dev/null || true)
[ -n "$GS" ] || { echo "  note: setsid gave no pid — the tree cannot be killed by group"; }

ready=0
for _ in $(seq 1 160); do
  [ -S "$RT/$NAME" ] && { ready=1; break; }
  [ -n "$GS" ] && ! kill -0 "$GS" 2>/dev/null && break
  sleep 0.5
done
if [ "$ready" = 1 ]; then
  ok "compositor socket $RT/$NAME, Wayland display name $NAME"
else
  bad "no Wayland socket — gnome-shell --headless did not come up"
  tail -15 "$RUN/gs.log" | sed 's/^/         /'
  exit 1
fi

# What the payload needs: the socket, the backend that reads it, and a runtime
# directory it can put its own nested compositor's socket in. The bus address is
# taken from the compositor's own environment so the payload reaches the portal
# started inside this session rather than the host's.
bus=$(tr '\0' '\n' < "/proc/$GS/environ" 2>/dev/null | grep '^DBUS_SESSION_BUS_ADDRESS=' || true)
cl_env=(
  "WAYLAND_DISPLAY=$NAME"
  "GDK_BACKEND=wayland"
  "XDG_RUNTIME_DIR=$RT"
  "HOME=$CHOME"
  "XDG_CONFIG_HOME=$CHOME/.config"
  "XDG_CACHE_HOME=$CHOME/.cache"
)
[ -n "$bus" ] && cl_env+=("$bus")

if [ "$#" -gt 0 ]; then
  echo "== payload: $*"
  env "${cl_env[@]}" CHROMIUM_HELPER="$HELPER" \
    timeout "$PAYLOAD_TIMEOUT" "$@" >"$RUN/payload.out" 2>"$RUN/payload.log"
  rc=$?
  cat "$RUN/payload.out"
  cat "$RUN/payload.log" >&2
  if [ $rc = 0 ]; then ok "payload exited 0 inside the nested session"
  else bad "payload exited $rc — its output is above, the compositor's log is below"
       tail -20 "$RUN/gs.log" | sed 's/^/         /'; fi
else
  {
    echo "load $PAGE"; echo "wait 2500"
    echo "js document.title"; echo "wait 800"; echo "quit"
  } > "$RUN/session"
  echo "== proof: a real window and a real page with nobody watching"
  env "${cl_env[@]}" CHROMIUM_HELPER="$HELPER" \
      CHROMIUM_SESSION_SCRIPT="$RUN/session" \
    timeout 180 "$DEMO" >"$RUN/demo.out" 2>"$RUN/demo.log"
  rc=$?
  [ $rc = 0 ] && ok "demo exited 0 against the nested compositor" \
              || bad "demo exited $rc — see the transcript below"
  # Three layers: the socket the helper was told about, the browser's own load
  # answer, and a value read back out of Blink. The script-step id counts every
  # line of the script, not every request, so it is matched, not pinned.
  for want in 'nested compositor on WAYLAND_DISPLAY=' 'HTTP 200' 'script [0-9]+ ok value="DND-PAGE'; do
    if grep -qE "$want" "$RUN/demo.out" "$RUN/demo.log"; then ok "transcript has /$want/"
    else bad "transcript lacks /$want/"; fi
  done
  # An abort in a sandboxed Chromium subprocess shows up here and nowhere else —
  # there is no crash handler in this tree (docs/sandbox-policy.md §8). It is
  # reported, not failed: straced, the aborter is a fork-without-exec child of
  # the zygote that self-terminates with tgkill(1, 1, SIGABRT) — pid 1 of its own
  # namespace, so libcef's code, not ours, whose own buffers are bounds-checked.
  # What is pinned instead is that it cost nothing: the three checks above are
  # the page still loading and answering.
  aborts=$(grep -c 'stack smashing' "$RUN/demo.out" "$RUN/demo.log" |
             awk -F: '{s+=$2} END{print s+0}')
  if [ "$aborts" = 0 ]; then ok "no subprocess aborted during the run"
  else ok "$aborts abort line(s) from a libcef sandbox child, and the page above survived it"; fi
fi

stop_compositor
left=$(pgrep -g "$GS" 2>/dev/null | wc -l)
if [ "$left" = 0 ]; then ok "nothing from the nested session is still running"
else bad "$left process(es) from the nested session survived the teardown"; fi

echo
if [ $fails = 0 ]; then printf 'ci session: passed\n'
else
  printf 'ci session: %d check(s) failed\n' "$fails"
  for f in demo payload; do
    for s in out log; do
      [ -s "$RUN/$f.$s" ] || continue
      echo "-- $f $s"; tail -60 "$RUN/$f.$s" | sed 's/^/  /'
    done
  done
  exit 1
fi
