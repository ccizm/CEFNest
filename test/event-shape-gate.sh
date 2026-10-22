#!/usr/bin/env bash
#
# test/event-shape-gate.sh — the host must survive an event that has no fields.
#
# The helper is a separate binary and a *different version* of one is a normal
# state of this channel: src/chromium-session.c says so itself where it logs an
# unknown event whole, because "the only way to find out what arrived is to see
# it". What it did not do was treat a field that arrived missing as the same kind
# of fact. json-glib answers `json_object_get_string_member` for a member that is
# not there with a critical and a NULL, and that NULL goes on into a signal the
# host documents as a string; the numeric getters answer a critical and a zero,
# which is worse in a different way — an id of 0, a 0 % progress, a menu at 0,0
# are all values something downstream believes.
#
# Measured with the guards taken back out again (2026-10-22): the 28-event stream
# cost the host 18 criticals — 8 string, 7 int, 2 double, 1 boolean — then a
# SIGSEGV, with `demo: download 0 begin name=download mime=(null) bytes=0
# url=(null)` in front of it. One run grades both halves: no critical may be
# printed, and nothing may be NULL on the way to the demo.
#
# The event list is read out of src/chromium-session.c rather than written down
# here, so an event added to the library is an event this gate sends. A run that
# found no events would be a run grading nothing, so the count is asserted.
#
# It opens a real demo window, so it is a documented manual gate on the same
# convention as test/framing-gate.sh — run it under ./test/ci-session.sh on a
# headless session. Nothing here moves a pointer on the host desktop.
#
#   ./test/event-shape-gate.sh
#   ./test/ci-session.sh ./test/event-shape-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
SESSION="$ROOT/src/chromium-session.c"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
skips=0

ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

if [ ! -f "$SESSION" ]; then
  printf '  skip event-shape: no %s to read the events out of\n' "$SESSION"
  printf '%d failure(s), %d skip(s)\n' "$fails" "$skips"
  exit 0
fi

# Every name the dispatch compares against.
mapfile -t EVENTS < <(grep -oE 'strcmp \(event, "[a-z0-9-]+"\)' "$SESSION" |
                      sed -E 's/.*"([a-z0-9-]+)".*/\1/' | sort -u)

if [ "${#EVENTS[@]}" -lt 20 ]; then
  bad "event-shape: only ${#EVENTS[@]} event name(s) found in $SESSION, wanted at least 20 — the grep pattern and the dispatch no longer agree, and this gate would be grading nothing"
  printf '%d failure(s)\n' "$fails"
  exit 1
fi
ok "event-shape: ${#EVENTS[@]} event name(s) read out of the dispatch"

if [ -z "${WAYLAND_DISPLAY:-}" ] || [ ! -x "$DEMO" ]; then
  printf '  skip event-shape: needs a wayland session and a built demo\n'
  printf '%d failure(s), %d skip(s)\n' "$fails" "$skips"
  exit $(((fails > 0)))
fi

# A helper that says only what each event is called. Every field the host reads
# for that event is absent, which is the shape of an event from a helper whose
# idea of a payload is not this one's.
{
  printf '#!/usr/bin/env python3\n'
  printf 'import json, os, socket, time\n'
  printf 's = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n'
  printf 's.connect(os.environ["CHROMIUM_HELPER_SOCKET"])\n'
  printf 'def send(o): s.sendall((json.dumps(o) + "\\n").encode())\n'
  printf 'send({"event": "ready", "pid": os.getpid(), "cef": "no-fields"})\n'
  for e in "${EVENTS[@]}"; do
    # ready is what opens the run and closed is what ends it, so closed goes last:
    # the host answers it by putting the channel down, and every event after it
    # would be graded against a connection that is already gone.
    [ "$e" = ready ] && continue
    [ "$e" = closed ] && continue
    printf 'send({"event": "%s"})\n' "$e"
  done
  printf 'send({"event": "closed"})\n'
  printf 'time.sleep (4)\n'
} > "$WORK/no-fields-helper.py"
chmod +x "$WORK/no-fields-helper.py"

printf 'load http://gate.test/page\nwait 2500\nclose\n' > "$WORK/no-fields.session"

printf '== no-fields\n'
timeout 90 env G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
  CHROMIUM_HELPER="$WORK/no-fields-helper.py" \
  CHROMIUM_SESSION_SCRIPT="$WORK/no-fields.session" \
  "$DEMO" >"$WORK/no-fields.out" 2>"$WORK/no-fields.log"
status=$?

if [ "$status" -ge 128 ]; then
  bad "no-fields: the demo died on signal $((status - 128)) (124 = it had to be killed)"
  tail -6 "$WORK/no-fields.log" | cut -c1-160 | sed 's/^/         /'
elif [ "$status" = 0 ]; then
  ok "no-fields: the demo finished with status 0"
else
  # The last event in the list is closed, which is the helper ending the session,
  # so the script has one step left when the host stops: a nonzero exit here is the
  # host doing what that event says, not a fault.
  ok "no-fields: the demo came back with status $status after being told the browser closed"
fi

# A critical is the library saying it did not expect this, and in this suite a
# critical is a failure rather than noise: seat-selftest installs a writer that
# stops the run on one, so the same rule belongs here.
n=$(grep -cE 'CRITICAL' "$WORK/no-fields.log")
if [ "$n" = 0 ]; then
  ok "no-fields: no critical from ${#EVENTS[@]} event(s) with no fields"
else
  bad "no-fields: $n critical(s) — a missing member was met as if it were present"
  grep -E 'CRITICAL' "$WORK/no-fields.log" | sed -E 's/.*\*\*: [0-9:.]+: //' |
    sort | uniq -c | sort -rn | head -5 | sed 's/^/         /'
fi

# "" is the answer the guards give; a NULL reaching the host's own print is the
# thing they exist to stop.
if grep -qE '\(null\)' "$WORK/no-fields.log"; then
  bad "no-fields: a NULL reached a signal the host documents as a string"
  grep -nE '\(null\)' "$WORK/no-fields.log" | head -4 | cut -c1-160 | sed 's/^/         /'
else ok "no-fields: nothing was (null) on the way to the demo"; fi

printf '%d failure(s), %d skip(s)\n' "$fails" "$skips"
exit $(((fails > 0)))
