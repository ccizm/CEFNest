#!/usr/bin/env bash
#
# test/framing-gate.sh — asserts the one-object-per-line rule on the wire.
#
# src/chromium-session.c:send_json() writes a command as a line, and the helper
# reads it as a line. Nothing in the tree grades that the two agree: the fake
# helper used to answer an unparsable line by dropping it, so a command that went
# out torn — and every command after it, because a stream does not resynchronise
# on its own — would have left the run green. This gate closes that gap from both
# ends.
#
#  1. A hand-built socket writes half a JSON object followed by a newline into the
#     fake. The fake has to notice: status 1, and a line saying so. That is what
#     makes the second half worth running — a gate cannot pass because the thing
#     it is watching for is invisible.
#  2. A "js" step whose code is larger than a socket buffer goes through the real
#     host, and the fake reports every command it read whole: the burst has to be
#     one line whose decoded length equals what was sent, and the "find" after it
#     has to come back answered from the far side. That is the framing asserted
#     rather than assumed.
#
# What half 2 grades is the rule, not the write loop in send_json(): a blocking
# GSocketConnection write does not come back short on this stack, measured — the
# loop there is the belt that keeps the promise if the transport ever does. Both
# halves were checked by breaking the thing they watch for: dropping the fake's
# hard failure makes half 1 green, and writing the JSON without its newline makes
# half 2 report zero whole burst lines and no answer after it.
#
# The first half needs nothing but python3. The second opens a demo window, so it
# is a documented manual gate on the same convention as test/find-gate.sh — run it
# under ./test/ci-session.sh on a headless session. Nothing here moves a pointer
# on the host desktop.
#
#   ./test/framing-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    tail -8 "$2" | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -n "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_count() { # expect_count <pattern> <logfile> <count> <tag>
  local n
  n=$(grep -cE "$1" "$2")
  if [ "$n" = "$3" ]; then ok "$4: $n line(s) match /$1/"
  else bad "$4: $n line(s) match /$1/, wanted $3"; fi
}

# The flood log carries a legal line of a megabyte of 'm' on one line, which is
# not something to paste into a terminal or into a report: the diagnosis is the
# pattern, and the payload can stay in the file.
expect_q() { # expect_q <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -aE 'session:|demo:|nested' "$2" | cut -c1-120 | tail -4 | sed 's/^/         /'
  fi
}

# ---------------------------------------------------------------------------
# 1. The fake is watching.
# ---------------------------------------------------------------------------

printf '== torn-line\n'
python3 - "$FAKE" "$WORK" <<'PYEOF'
import os
import socket
import subprocess
import sys
import tempfile

fake, work = sys.argv[1], sys.argv[2]
path = os.path.join(tempfile.mkdtemp(dir=work), "sock")

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)

proc = subprocess.Popen([sys.executable, fake],
                        env=dict(os.environ, CHROMIUM_HELPER_SOCKET=path),
                        stderr=subprocess.PIPE, text=True)
conn, _ = server.accept()
# A real command cut short, then a newline: the shape of a write that did not
# finish. The fake must not read this as "a command I do not care about".
conn.sendall(b'{"cmd": "js", "code": "abc\n')
conn.sendall(b'{"cmd": "find", "pattern": "e"}\n')

try:
    status = proc.wait(timeout=15)
    log = proc.stderr.read()
except subprocess.TimeoutExpired:
    proc.kill()
    status, log = -1, "the fake is still alive after a torn command"

with open(os.path.join(work, "torn-line.log"), "w") as sink:
    sink.write(log)
    sink.write("exit status %d\n" % status)

sys.exit(0 if status == 1 and "unparsable" in log else 1)
PYEOF
if [ $? -eq 0 ]; then
  ok "torn-line: the fake stopped the run (status 1, and said why)"
else
  bad "torn-line: the fake swallowed a half-written command — /unparsable/ is not being reported"
  tail -8 "$WORK/torn-line.log" 2>/dev/null | sed 's/^/         /'
fi

# ---------------------------------------------------------------------------
# 2. A burst crosses the socket whole.
# ---------------------------------------------------------------------------

if [ -z "${WAYLAND_DISPLAY:-}" ] || [ ! -x "$DEMO" ]; then
  # The `  skip ` prefix is what ci-all.sh counts, so a burst this machine cannot
  # open shows up in its table instead of vanishing from a passing line.
  printf '  skip burst: needs a wayland session and a built demo\n'
  printf '  skip flood: needs a wayland session and a built demo\n'
  printf '  skip eof: needs a wayland session and a built demo\n'
  printf '%d failure(s)\n' "$fails"
  exit $(((fails > 0)))
fi

# Above the 208 KiB default send buffer of an AF_UNIX stream, so one command has
# to be carried by more than one transfer and drained as the reader takes it.
BURST=300000

# 1.5 s rather than the 250 ms this started with. The two reds at 250 ms were first
# blamed on a slow machine and that was the wrong answer: the fake was stranding the
# command *behind* the burst in Python's own read buffer while select() watched the
# kernel's (fixed in test/fake-helper.py, which now drains one buffer by hand). The
# width stays because this half asserts that the far side answers, and an answer
# that arrives in one dispatch is still an answer that has to be waited for.
cat > "$WORK/burst.session" <<EOF
load http://gate.test/page
wait 700
js $(head -c $BURST /dev/zero | tr '\0' 'a')
find e
wait 1500
close
EOF

printf '== burst\n'
if timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$FAKE" \
     CHROMIUM_SESSION_SCRIPT="$WORK/burst.session" \
     "$DEMO" >"$WORK/burst.out" 2>"$WORK/burst.log"; then
  ok "burst: the demo finished with status 0"
else
  bad "burst: the demo exited with $? — see $WORK/burst.log"
  tail -8 "$WORK/burst.log" | sed 's/^/         /'
fi

# Exactly one whole burst line: a write that stopped short would put two halves,
# each with a shorter code= than was sent, and the count would not reach 1.
expect_count "framing cmd=js line=[0-9]+ code=$BURST" "$WORK/burst.log" 1 burst
# A torn write desyncs the stream, so the command after the burst has to be
# answered *by the far side*: demo: logs the step being dispatched locally, but a
# find result only appears if the fake parsed the line the host wrote.
expect 'find result id=[0-9]+ count=6 ordinal=[0-9]+ final=1' "$WORK/burst.log" burst
expect_no 'unparsable' "$WORK/burst.log" burst

# ---------------------------------------------------------------------------
# 3. The host's half of the same rule: a line has to end.
# ---------------------------------------------------------------------------

# src/chromium-session.c frames what it *reads* the same way it frames what it
# writes, and a peer that stops sending newlines is asking for an allocation the
# size of its own imagination unless the host says stop. One run grades both
# directions of the ceiling: a legal big line that has to survive, and a line with
# no end that must not be allowed to grow.
cat > "$WORK/flood-helper.py" <<'EOF'
#!/usr/bin/env python3
import json
import os
import socket
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.environ['CHROMIUM_HELPER_SOCKET'])


def send(obj):
    sock.sendall((json.dumps(obj) + '\n').encode())


send({'event': 'ready', 'pid': os.getpid(), 'cef': 'flood'})
# A js result is as big as whatever the page returned, so a line this size is
# ordinary traffic and has to be taken whole.
send({'event': 'status', 'message': 'm' * 1000000})
# A NUL is not a character in this protocol: JSON has no way to carry one, so a
# line holding it is not an event with something odd in it. The shape that hurts is
# a *complete* object in front of the NUL — every string-based reader then answers
# with that object and throws away everything after it, which is a plausible event
# the helper never sent and no complaint anywhere.
sock.sendall(b'{"event": "status", "message": "before"}\x00after\n')
# And this one never ends.
sock.sendall(b'x' * (5 * 1024 * 1024))
time.sleep (3)
EOF
chmod +x "$WORK/flood-helper.py"

cat > "$WORK/flood.session" <<EOF
load http://gate.test/page
wait 1500
close
EOF

printf '== flood\n'
timeout --kill-after=10s 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$WORK/flood-helper.py" \
  CHROMIUM_SESSION_SCRIPT="$WORK/flood.session" \
  "$DEMO" >"$WORK/flood.out" 2>"$WORK/flood.log"
status=$?
# The exit status is not the assertion — which of the two ways this run can end is
# told by what is in the log — but a demo that had to be killed is worth seeing.
if [ "$status" = 0 ] || [ "$status" = 1 ]; then
  ok "flood: the demo came back on its own (status $status)"
else
  bad "flood: the demo exited with $status (124 = it had to be killed) — see $WORK/flood.log"
  grep -acE '.' "$WORK/flood.log" | sed 's/^/         lines in the log: /'
fi

expect_q 'over the [0-9]+ byte ceiling' "$WORK/flood.log" flood
expect_q 'helper stopped \(control line too long\)' "$WORK/flood.log" flood

# The big legal line has to arrive whole: 1 000 000 characters after "helper
# status: ", not a truncation of it and not a ceiling complaint about it.
big=$(grep -o 'helper status: m*' "$WORK/flood.log" | awk '{ print length($0) - 15 }' | head -1)
if [ "${big:-0}" = "1000000" ]; then
  ok "flood: the 1000000-byte legal line was read whole"
else
  bad "flood: the legal line came back ${big:-nothing}, wanted 1000000 characters"
fi

# The NUL line has to be refused *and* not half-believed: "before" is a complete
# JSON object on its own, so a host that truncates at the NUL reports an event the
# helper never sent.
expect_q 'control line holds a NUL at [0-9]+ of [0-9]+' "$WORK/flood.log" flood
if grep -qaE 'helper status: before' "$WORK/flood.log"; then
  bad "flood: the bytes before a NUL were read as an event"
  grep -anE 'helper status: before' "$WORK/flood.log" | cut -c1-120 | sed 's/^/         /'
else ok "flood: nothing was handled out of the NUL line"; fi

# ---------------------------------------------------------------------------
# 4. A channel that has ended must not be read again.
# ---------------------------------------------------------------------------

# The read loop is asynchronous: its callback re-arms itself after every chunk.
# A peer that closes the socket without exiting the process leaves the host with
# an EOF that arrives immediately and forever — the child is still alive, so the
# reaping path never runs to drop the connection. The assertion is the host's own
# CPU time: re-arming a dead read burns a core, while a dropped channel idles.
cat > "$WORK/eof-helper.py" <<'EOF'
#!/usr/bin/env python3
import json
import os
import socket
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.environ['CHROMIUM_HELPER_SOCKET'])
sock.sendall((json.dumps({'event': 'ready', 'pid': os.getpid(),
                          'cef': 'eof'}) + '\n').encode())
time.sleep(0.2)
# The channel ends; the process does not. This is the shape of a helper that
# has given up on its control socket but is still running.
sock.close()
time.sleep(8)
EOF
chmod +x "$WORK/eof-helper.py"

cat > "$WORK/eof.session" <<EOF
# The wait is the last step on purpose: helper-gone only ends a run whose
# script still has steps left, and the run has to stay alive while its CPU is
# measured. The wait timer ends it.
load http://gate.test/page
wait 6000
EOF

printf '== eof\n'
env GDK_BACKEND=wayland CHROMIUM_HELPER="$WORK/eof-helper.py" \
  CHROMIUM_SESSION_SCRIPT="$WORK/eof.session" \
  "$DEMO" >"$WORK/eof.out" 2>"$WORK/eof.log" &
eof_pid=$!

# Two seconds in, the channel has closed. Measure the next two seconds of CPU:
# a read loop that re-arms on EOF spends them, a dropped channel does not.
sleep 2
before=$(awk '{ print $14 + $15 }' "/proc/$eof_pid/stat" 2>/dev/null || echo 0)
sleep 2
after=$(awk '{ print $14 + $15 }' "/proc/$eof_pid/stat" 2>/dev/null || echo 0)
used=$((after - before))
wait "$eof_pid"
eof_status=$?

if [ "$used" -lt 100 ]; then
  ok "eof: the host used $used CPU tick(s) in two seconds after the peer closed"
else
  bad "eof: the host used $used CPU tick(s) in two seconds after the peer closed — the read loop is re-arming on EOF"
  tail -8 "$WORK/eof.log" | sed 's/^/         /'
fi
# A helper that vanishes in the middle of a script ends the run by design; what
# this run grades is that the host noticed, not which status the script chose.
if grep -qaE 'the helper went away|helper stopped' "$WORK/eof.log" "$WORK/eof.out" 2>/dev/null; then
  ok "eof: the host noticed the channel end"
else
  bad "eof: the host never reported the channel end — see $WORK/eof.log"
  tail -8 "$WORK/eof.log" | sed 's/^/         /'
fi
[ "$eof_status" = 0 ] || [ "$eof_status" = 1 ] || \
  bad "eof: the demo exited with $eof_status — see $WORK/eof.log"

printf '%d failure(s)\n' "$fails"
exit $(((fails > 0)))
