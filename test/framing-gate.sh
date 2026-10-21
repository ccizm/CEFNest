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
  printf '%d failure(s)\n' "$fails"
  exit $(((fails > 0)))
fi

# Above the 208 KiB default send buffer of an AF_UNIX stream, so one command has
# to be carried by more than one transfer and drained as the reader takes it.
BURST=300000

cat > "$WORK/burst.session" <<EOF
load http://gate.test/page
wait 700
js $(head -c $BURST /dev/zero | tr '\0' 'a')
find e
wait 250
close
EOF

printf '== burst\n'
if timeout 90 env GDK_BACKEND=wayland CHROMIUM_HELPER="$FAKE" \
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

printf '%d failure(s)\n' "$fails"
exit $(((fails > 0)))
