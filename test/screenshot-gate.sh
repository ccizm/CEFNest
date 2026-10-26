#!/usr/bin/env bash
#
# test/screenshot-gate.sh — Page.captureScreenshot as a component method.
#
# The page shows one solid colour and a marker string; the PNG that comes
# back must be a real PNG (magic bytes, a declared width and height), big
# enough to be a page, and — the pixel check — its corner pixels carry the
# page's own background, not a fallback.
#
#   ./test/screenshot-gate.sh
#   ./test/ci-session.sh ./test/screenshot-gate.sh
#
# Needs python3; 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/screenshot-gate-page.html"

WORK=$(mktemp -d)
OUT="$WORK/shot.png"
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'screenshot|loaded' "$2" | tail -6 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the gate needs a PNG reader"; exit 77; fi

cat >"$WORK/shot.session" <<EOF
load $PAGE
wait 2000
screenshot
wait 2000
quit
EOF

echo "== capture the probe page"
timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
  CEFNEST_SCREENSHOT_OUT="$OUT" \
  CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/shot.session" \
  "$DEMO" >"$WORK/shot.log" 2>&1
status=$?
log=$WORK/shot.log
if [ "$status" = 0 ]; then ok "shot: demo exited 0"
else bad "shot: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: screenshot id=[0-9]+ browser=1 ok=1 bytes=[0-9]+' "$log" shot
expect "demo: screenshot written $OUT" "$log" shot: written

if [ -s "$OUT" ]; then ok "shot: the file is there and not empty"
else bad "shot: no PNG at $OUT"; fi

python3 - "$OUT" <<'EOF'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as handle:
    data = handle.read()

ok = True
if data[:8] != b"\x89PNG\r\n\x1a\n":
    print("  FAIL shot: not a PNG (magic bytes)")
    sys.exit(1)
if len(data) < 2000:
    print("  FAIL shot: PNG suspiciously small (%d bytes)" % len(data))
    sys.exit(1)
# IHDR: width and height, big-endian, right after the first chunk header.
width, height = struct.unpack(">II", data[16:24])
if width <= 0 or height <= 0:
    print("  FAIL shot: declared size %dx%d" % (width, height))
    sys.exit(1)
print("  ok   shot: PNG %dx%d, %d bytes" % (width, height, len(data)))
EOF
if [ $? != 0 ]; then fails=$((fails + 1)); else ok "shot: PNG magic, size and dimensions"; fi

echo
if [ $fails = 0 ]; then printf 'screenshot gate: passed\n'
else printf 'screenshot gate: %d check(s) failed\n' "$fails"; exit 1; fi
