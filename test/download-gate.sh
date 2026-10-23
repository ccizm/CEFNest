#!/usr/bin/env bash
#
# test/download-gate.sh — drives the whole download path and asserts on it.
#
# Five runs against test/fake-helper.py cover the host-side layers (the panel's
# policy, the accept/reject calls, the name sanitising and the duplicate-name
# uniquifying), and three more against the real CEF helper cover the browser-side
# half: an attachment response becomes download-begin, an accepted path reaches
# the disk, and both routes of "no" end in a cancelled download with no file.
#
# It opens a real demo window per run, so it is a documented manual gate rather
# than a meson test — the same convention as test/*.session. Nothing here moves
# a pointer on the host desktop.
#
#   ./test/download-gate.sh            # fake + real
#   ./test/download-gate.sh fake       # only the headless-ish half
#   ./test/download-gate.sh real       # only the CEF half

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"
. "$ROOT/test/helper-path.sh"
WHICH=${1:-all}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -n 'download' "$2" | head -8 | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -n "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_file() { # expect_file <path> <tag> <byte-count>
  local size
  if [ ! -f "$1" ]; then bad "$2: no file at $1"; return; fi
  size=$(stat -c%s "$1")
  if [ "$size" = "$3" ]; then ok "$2: $(basename "$1") is $size bytes"
  else bad "$2: $(basename "$1") is $size bytes, wanted $3"; fi
}

expect_no_file() { # expect_no_file <path> <tag>
  if [ -e "$1" ]; then bad "$2: $1 should not exist"
  else ok "$2: nothing landed"; fi
}

expect_count() { # expect_count <pattern> <logfile> <count> <tag>
  local n
  n=$(grep -cE "$1" "$2")
  if [ "$n" = "$3" ]; then ok "$4: $n line(s) match /$1/"
  else bad "$4: $n line(s) match /$1/, wanted $3"; fi
}

expect_empty_dir() { # expect_empty_dir <path> <tag>
  if [ ! -d "$1" ]; then bad "$2: no directory at $1"; return; fi
  if [ -z "$(ls -A "$1")" ]; then ok "$2: $1 is empty"
  else bad "$2: $1 holds $(ls -A "$1" | tr '\n' ' ')"; fi
}

run_demo() { # run_demo <tag> <logfile> <helper> <extra env...>
  local tag=$1 log=$2 helper=$3
  shift 3

  if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
  echo "== $tag"
  timeout 90 env "$@" GDK_BACKEND=wayland CHROMIUM_HELPER="$helper" \
    CHROMIUM_SESSION_SCRIPT="$WORK/$tag.session" \
    "$DEMO" >"$WORK/$tag.out" 2>"$log"
  local status=$?
  if [ $status -ne 0 ]; then
    bad "$tag: the demo exited with $status"
    tail -6 "$log" | sed 's/^/         /'
  fi
}

script() { # script <tag> writes the heredoc that follows into the tag's script
  cat > "$WORK/$1.session"
}

# ---------------------------------------------------------------------------
# The fake helper: the host's own layers, with the timings under our control.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = fake ]; then
  if [ ! -f "$FAKE" ]; then echo "no fake helper at $FAKE"; exit 1; fi

  # 1. The default policy: accept into CEFNEST_DOWNLOAD_DIR, twice for the name.
  script fake-auto <<EOF
load http://gate.test/downloads/fake?name=auto.txt&bytes=3000
wait 900
load http://gate.test/downloads/fake?name=auto.txt&bytes=3000
wait 1400
close
EOF
  run_demo fake-auto "$WORK/fake-auto.log" "$FAKE" \
    CEFNEST_DOWNLOAD_DIR="$WORK/auto"
  expect 'download 1 begin name=auto\.txt .*bytes=3000' "$WORK/fake-auto.log" fake-auto
  expect 'download 1 accept path=.*/auto/auto\.txt' "$WORK/fake-auto.log" fake-auto
  expect 'download 1 progress percent=[0-9]+' "$WORK/fake-auto.log" fake-auto
  expect 'download 1 end state=complete' "$WORK/fake-auto.log" fake-auto
  expect 'download 2 accept path=.*/auto/auto \(1\)\.txt' "$WORK/fake-auto.log" \
    fake-auto
  expect 'download 2 end state=complete' "$WORK/fake-auto.log" fake-auto
  expect_file "$WORK/auto/auto.txt" fake-auto 3000
  expect_file "$WORK/auto/auto (1).txt" fake-auto 3000

  # 2. A page that names a path instead of a file: only the last component is
  #    used, so the host's directory cannot be escaped.
  script fake-escape <<EOF
load http://gate.test/downloads/fake?name=../../outside.txt&bytes=1024
wait 1200
close
EOF
  run_demo fake-escape "$WORK/fake-escape.log" "$FAKE" \
    CEFNEST_DOWNLOAD_DIR="$WORK/escape"
  expect 'download 1 begin name=outside\.txt' "$WORK/fake-escape.log" fake-escape
  expect 'download 1 accept path=.*/escape/outside\.txt' "$WORK/fake-escape.log" fake-escape
  expect_file "$WORK/escape/outside.txt" fake-escape 1024
  expect_no_file "$WORK/outside.txt" fake-escape
  expect_no_file "$WORK/../outside.txt" fake-escape

  # 3. CEFNEST_DOWNLOAD_ASK=1: the row waits, and the script step stands in for
  #    the 保存 button — including a path outside the policy directory.
  script fake-ask <<EOF
load http://gate.test/downloads/fake?name=asked.bin&bytes=2048
wait 400
download-accept $WORK/manual/deep/nested/asked.bin
wait 1200
close
EOF
  run_demo fake-ask "$WORK/fake-ask.log" "$FAKE" CEFNEST_DOWNLOAD_ASK=1
  expect 'download 1 begin name=asked\.bin' "$WORK/fake-ask.log" fake-ask
  expect 'download 1 accept path=.*/manual/deep/nested/asked\.bin' \
    "$WORK/fake-ask.log" fake-ask
  expect 'download 1 end state=complete' "$WORK/fake-ask.log" fake-ask
  expect_file "$WORK/manual/deep/nested/asked.bin" fake-ask 2048

  # 4. Refusing before it starts: one cancelled end, and no file anywhere.
  script fake-refuse <<EOF
load http://gate.test/downloads/fake?name=nope.bin&bytes=4096
wait 400
download-reject
wait 600
close
EOF
  run_demo fake-refuse "$WORK/fake-refuse.log" "$FAKE" \
    CEFNEST_DOWNLOAD_DIR="$WORK/refuse" CEFNEST_DOWNLOAD_ASK=1
  expect 'download 1 reject' "$WORK/fake-refuse.log" fake-refuse
  expect 'download 1 end state=cancelled' "$WORK/fake-refuse.log" fake-refuse
  expect_no 'state=complete' "$WORK/fake-refuse.log" fake-refuse
  expect_no_file "$WORK/refuse/nope.bin" fake-refuse

  # 5. Cancelling mid-flight: the same call, and the partial file goes away.
  script fake-cancel <<EOF
load http://gate.test/downloads/fake?name=big.bin&bytes=40000
wait 200
download-accept $WORK/cancel/big.bin
wait 300
download-reject
wait 800
close
EOF
  run_demo fake-cancel "$WORK/fake-cancel.log" "$FAKE" CEFNEST_DOWNLOAD_ASK=1
  expect 'download 1 progress percent=[1-9]' "$WORK/fake-cancel.log" fake-cancel
  expect 'download 1 end state=cancelled' "$WORK/fake-cancel.log" fake-cancel
  expect_no 'state=complete' "$WORK/fake-cancel.log" fake-cancel
  expect_no_file "$WORK/cancel/big.bin" fake-cancel

  # 6. Never answered at all: the download stays parked, and the one promise the
  #    protocol makes about that is that no file was written for it.
  script fake-never <<EOF
load http://gate.test/downloads/fake?name=ignored.bin&bytes=2048
wait 1200
close
EOF
  run_demo fake-never "$WORK/fake-never.log" "$FAKE" \
    CEFNEST_DOWNLOAD_ASK=1 CEFNEST_DOWNLOAD_DIR="$WORK/never"
  expect 'download 1 begin name=ignored\.bin' "$WORK/fake-never.log" fake-never
  expect_no 'download 1 (accept|reject)' "$WORK/fake-never.log" fake-never
  expect_no 'end state=' "$WORK/fake-never.log" fake-never
  expect_empty_dir "$WORK/never" fake-never
fi

# ---------------------------------------------------------------------------
# The real helper: Chromium's own download item behind the same five messages.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = real ]; then
  if [ ! -x "$HELPER" ]; then
    echo "no helper at $HELPER — run cmake --build helper/build --config Release"
    exit 1
  fi

  PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

  cat > "$WORK/server.py" <<'PY'
import http.server
import socketserver
import sys
import time

# gate.bin is 6000 bytes and answers in one write, so the small attachment is
# long finished before the run looks at it. stream.bin is 2 MiB dribbled out at
# 32 KiB per 100 ms: on a loopback address that is the only way a download can
# still be in flight when something tries to cancel it. second.bin is the
# second attachment the named-accept scenario runs next to a live stream.
BLOB = b"cefnest download gate payload\n" * 200
STREAM = b"x" * (2 * 1024 * 1024)
SECOND = b"s" * 4321
CHUNK = 32 * 1024


class Handler(http.server.BaseHTTPRequestHandler):
    def _headers(self, name, length):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.send_header("Content-Length", str(length))
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/blob2"):
            self._headers("second.bin", len(SECOND))
            self.wfile.write(SECOND)
        elif self.path.startswith("/blob"):
            self._headers("gate.bin", len(BLOB))
            self.wfile.write(BLOB)
        elif self.path.startswith("/stream"):
            self._headers("stream.bin", len(STREAM))
            for off in range(0, len(STREAM), CHUNK):
                try:
                    self.wfile.write(STREAM[off:off + CHUNK])
                    self.wfile.flush()
                    time.sleep(0.1)
                except (BrokenPipeError, ConnectionResetError):
                    return
        else:
            body = b"<html><body><p>cefnest download gate</p></body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


with Server(("127.0.0.1", int(sys.argv[1])), Handler) as httpd:
    httpd.serve_forever()
PY
  python3 "$WORK/server.py" "$PORT" &
  server_pid=$!
  trap 'kill $server_pid 2>/dev/null; rm -rf "$WORK"' EXIT

  # An attachment response has to reach the host as a download, and an accepted
  # path has to be the file Chromium really wrote.
  script real-auto <<EOF
load http://127.0.0.1:$PORT/blob
wait 9000
close
EOF
  run_demo real-auto "$WORK/real-auto.log" "$HELPER" \
    CEFNEST_DOWNLOAD_DIR="$WORK/real"
  expect 'download [0-9]+ begin name=gate\.bin .*bytes=6000' "$WORK/real-auto.log" real-auto
  expect 'download [0-9]+ progress percent=[0-9]+' "$WORK/real-auto.log" real-progress
  expect 'download [0-9]+ end state=complete path=.*/real/gate\.bin' \
    "$WORK/real-auto.log" real-auto
  # The gate's first progress event used to arrive before download-begin, for an
  # id the host had never been told about; one begin per run proves it cannot.
  # Chromium's ids come out of its download database, so they keep counting up
  # across runs from the same cache directory.
  expect_count 'begin name=gate\.bin' "$WORK/real-auto.log" 1 real-auto
  expect_file "$WORK/real/gate.bin" real-auto 6000

  # Refusing without ever naming a path: nothing of this run's may exist in the
  # directory, and the host must be told exactly once.
  script real-refuse <<EOF
load http://127.0.0.1:$PORT/blob
wait 6000
download-reject
wait 3000
close
EOF
  run_demo real-refuse "$WORK/real-refuse.log" "$HELPER" \
    CEFNEST_DOWNLOAD_ASK=1 CEFNEST_DOWNLOAD_DIR="$WORK/refuse"
  expect 'download [0-9]+ begin name=gate\.bin' "$WORK/real-refuse.log" real-refuse
  expect 'download [0-9]+ reject' "$WORK/real-refuse.log" real-refuse
  expect_no 'download [0-9]+ accept' "$WORK/real-refuse.log" real-refuse
  expect_count 'end state=cancelled' "$WORK/real-refuse.log" 1 real-refuse
  expect_no 'state=complete' "$WORK/real-refuse.log" real-refuse
  expect_empty_dir "$WORK/refuse" real-refuse

  # Cancelling in flight: 2 MiB at 32 KiB per 100 ms takes about six seconds to
  # arrive, and Chromium keeps reading ahead of the file write, so the reject two
  # seconds after the path is handed over still reaches a live callback.
  script real-cancel <<EOF
load http://127.0.0.1:$PORT/stream
wait 1500
download-accept $WORK/cancel/stream.bin
wait 2000
download-reject
wait 3000
close
EOF
  run_demo real-cancel "$WORK/real-cancel.log" "$HELPER" \
    CEFNEST_DOWNLOAD_ASK=1 CEFNEST_DOWNLOAD_DIR="$WORK/cancel"
  expect 'download [0-9]+ progress percent=([1-9]|[1-9][0-9]) received' \
    "$WORK/real-cancel.log" real-cancel
  expect 'download [0-9]+ end state=cancelled' "$WORK/real-cancel.log" real-cancel
  expect_no 'state=complete' "$WORK/real-cancel.log" real-cancel
  echo "  info real-cancel: $(ls -A "$WORK/cancel" | tr '\n' ' ')"

  # Two downloads alive at once: one parked, one accepted and in flight. The
  # positional steps always reach the first row, so this run is the one that
  # proves the named steps can say which download they mean. Second.bin goes to
  # its own path by name; the in-flight stream is cancelled by name and may not
  # leave anything behind.
  #
  # The last wait is long on purpose. Second.bin is on disk the moment its path
  # is named (measured: mtime lands with the accept line), but CEF only reports
  # the terminal complete 3.2-5.7 s later — measured three times on this
  # scenario — because the final state update goes out with Chromium's batched
  # download-database flush rather than with the move. A 4 s window lost the line
  # while the file was there.
  script real-named <<EOF
load http://127.0.0.1:$PORT/stream
wait 1500
download-accept $WORK/named/stream.bin
load http://127.0.0.1:$PORT/blob2
wait 1500
download-accept-name second $WORK/named/second.bin
download-reject-name stream
wait 9000
close
EOF
  run_demo real-named "$WORK/real-named.log" "$HELPER" \
    CEFNEST_DOWNLOAD_ASK=1 CEFNEST_DOWNLOAD_DIR="$WORK/named"
  expect 'download [0-9]+ begin name=stream\.bin' "$WORK/real-named.log" real-named
  expect 'download [0-9]+ begin name=second\.bin' "$WORK/real-named.log" real-named
  expect 'download [0-9]+ end state=complete path=.*/named/second\.bin' \
    "$WORK/real-named.log" real-named
  expect 'download [0-9]+ end state=cancelled' "$WORK/real-named.log" real-named
  expect_count 'end state=cancelled' "$WORK/real-named.log" 1 real-named
  expect_count 'state=complete' "$WORK/real-named.log" 1 real-named
  expect_file "$WORK/named/second.bin" real-named 4321
  if [ -e "$WORK/named/stream.bin" ]; then
    bad "real-named: stream.bin landed in $WORK/named"
  else
    ok "real-named: no stream.bin in $WORK/named"
  fi

  # Never answered: CEF cancels the download when the parked gate dies with the
  # process, so the run that walks away from a request leaves nothing behind.
  script real-never <<EOF
load http://127.0.0.1:$PORT/blob
wait 6000
close
EOF
  run_demo real-never "$WORK/real-never.log" "$HELPER" \
    CEFNEST_DOWNLOAD_ASK=1 CEFNEST_DOWNLOAD_DIR="$WORK/never"
  expect 'download [0-9]+ begin name=gate\.bin' "$WORK/real-never.log" real-never
  expect_no 'download [0-9]+ (accept|reject)' "$WORK/real-never.log" real-never
  expect_empty_dir "$WORK/never" real-never
fi

echo
if [ $fails -eq 0 ]; then
  echo "download gate: all checks passed"
else
  echo "download gate: $fails check(s) failed"
fi
exit $((fails > 0))
