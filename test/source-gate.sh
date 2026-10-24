#!/usr/bin/env bash
#
# test/source-gate.sh — View Source, graded without a browser.
#
# The fake helper streams the same events the real one does for the menu's
# 查看网页源代码 (and the chromium_session_view_source() call): begin, chunk*,
# end — or error. This gate drives the demo's own "view-source" script step
# against every shape and grades what the component does with it:
#
#   small       one chunk, exact bytes and a g_str_hash of the content
#   multibyte   several chunks, every boundary inside the multibyte run —
#               byte count AND hash must match, which only survives correct
#               reassembly
#   huge        a lying helper (17 MiB, no truncated flag): the session's own
#               16 MiB cap is the assertion
#   error       view-source-error, no window
#   slow        two requests: one window created, the second reuses it
#   no-window   auto-source-window=FALSE: the signal, never a window
#   torn-down   "close" mid-stream: the half-transfer dies quietly
#
# It opens a real demo window, so it runs under ./test/ci-session.sh on a
# headless session (a plain GTK window maps there — the popover limitation
# that skips menu-gate does not apply). Nothing here moves a pointer on the
# host desktop.
#
#   ./test/source-gate.sh
#   ./test/ci-session.sh ./test/source-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

if [ ! -x "$DEMO" ]; then
  echo "source gate: SKIP (no demo binary; build with meson)"
  exit 77
fi

# g_str_hash re-implemented exactly: h = h * 33 + c with h0 = 5381 — and c is
# a *signed* char in GLib, so a byte at 0x80 and above comes in negative.
# Pinned against the library itself (cc + g_str_hash), 2026-10-25.
ghash() {
  python3 - "$1" <<'EOF'
import sys
h = 5381
for byte in sys.argv[1].encode("utf-8"):
    h = (h * 33 + (byte if byte < 128 else byte - 256)) & 0xFFFFFFFF
print(h)
EOF
}

# run <script-file> [env...] — one demo run against the fake helper, its
# stderr in $WORK/log.
run() {
  local script="$1"
  shift
  env "$@" \
    CHROMIUM_HELPER="$FAKE" \
    CHROMIUM_SESSION_SCRIPT="$script" \
    G_MESSAGES_DEBUG="chromium-session" \
    "$DEMO" >"$WORK/log" 2>&1
  echo $?
}

line_count() { grep -c "$1" "$WORK/log" 2>/dev/null || true; }

# ---------------------------------------------------------------- small
cat >"$WORK/small.session" <<'EOF'
load http://source.test/page?source=small
wait 800
view-source
wait 800
close
EOF

SMALL='<html><head><title>small</title></head><body>cefnest fake source</body></html>'
SMALL_BYTES=$(printf %s "$SMALL" | wc -c)
SMALL_HASH=$(ghash "$SMALL")

rc=$(run "$WORK/small.session")
if [ "$rc" = 0 ] &&
   grep -q "demo: view-source url=http://source.test/page?source=small bytes=$SMALL_BYTES hash=$SMALL_HASH" "$WORK/log"; then
  ok "small: one request, $SMALL_BYTES bytes, content hash matches"
else
  bad "small: wanted bytes=$SMALL_BYTES hash=$SMALL_HASH, got:"
  grep "demo: view-source" "$WORK/log" || echo "       (no ::view-source line at all)"
fi
if [ "$(line_count 'source window: created')" = 1 ] &&
   [ "$(line_count 'source window: shown .*truncated=0')" = 1 ]; then
  ok "small: the component's own window opened"
else
  bad "small: window debug lines missing (created/shown)"
fi

# ------------------------------------------------------------ multibyte
cat >"$WORK/multi.session" <<'EOF'
load http://source.test/page?source=multibyte
wait 800
view-source
wait 1200
close
EOF

MULTI='<html><body>中文内容'"$(python3 -c "print('汉字测试🎉序列' * 3000)")"'结尾</body></html>'
MULTI_BYTES=$(printf %s "$MULTI" | wc -c)
MULTI_HASH=$(ghash "$MULTI")

rc=$(run "$WORK/multi.session")
if [ "$rc" = 0 ] &&
   grep -q "demo: view-source .*bytes=$MULTI_BYTES hash=$MULTI_HASH" "$WORK/log"; then
  ok "multibyte: $MULTI_BYTES bytes across chunked multibyte runs, hash matches"
else
  bad "multibyte: wanted bytes=$MULTI_BYTES hash=$MULTI_HASH, got:"
  grep "demo: view-source" "$WORK/log" || echo "       (no ::view-source line at all)"
fi

# ----------------------------------------------------------------- huge
cat >"$WORK/huge.session" <<'EOF'
load http://source.test/page?source=huge
wait 800
view-source
wait 3000
close
EOF

CAP=16777216
rc=$(run "$WORK/huge.session")
if [ "$rc" = 0 ] &&
   grep -q "demo: view-source .*bytes=$CAP " "$WORK/log"; then
  ok "huge: the session capped a lying 17 MiB stream at its own $CAP"
else
  bad "huge: wanted the session's own cap $CAP, got:"
  grep "demo: view-source" "$WORK/log" || echo "       (no ::view-source line at all)"
fi

# ---------------------------------------------------------------- error
cat >"$WORK/error.session" <<'EOF'
load http://source.test/page?source=error
wait 800
view-source
wait 800
close
EOF

rc=$(run "$WORK/error.session")
if [ "$rc" = 0 ] &&
   grep -q "demo: view-source-error browser=1 the frame has no source" "$WORK/log" &&
   [ "$(line_count 'source window: shown')" = 0 ]; then
  ok "error: ::view-source-error, no window"
else
  bad "error: wanted the error line and no window"
fi

# ------------------------------------------- two requests, one window
cat >"$WORK/reuse.session" <<'EOF'
load http://source.test/page?source=slow
wait 800
view-source
wait 2500
view-source
wait 2500
close
EOF

rc=$(run "$WORK/reuse.session")
created=$(line_count 'source window: created')
reused=$(line_count 'source window: reused')
signals=$(line_count 'demo: view-source url=')
if [ "$rc" = 0 ] && [ "$created" = 1 ] && [ "$reused" = 1 ] && [ "$signals" = 2 ]; then
  ok "slow/reuse: two requests, one window created, one reused"
else
  bad "slow/reuse: created=$created reused=$reused signals=$signals (wanted 1/1/2)"
fi

# -------------------------------------------- auto-source-window = FALSE
cat >"$WORK/nowin.session" <<'EOF'
load http://source.test/page?source=small
wait 800
view-source
wait 800
close
EOF

rc=$(run "$WORK/nowin.session" CEFNEST_SOURCE_WINDOW=0)
if [ "$rc" = 0 ] &&
   grep -q "demo: view-source url=" "$WORK/log" &&
   [ "$(line_count 'source window: shown')" = 0 ]; then
  ok "no-window: auto-source-window=FALSE keeps the signal, drops the window"
else
  bad "no-window: a window opened or the signal went missing"
fi

# ------------------------------------------------- close mid-transfer
cat >"$WORK/tear.session" <<'EOF'
load http://source.test/page?source=slow
wait 800
view-source
wait 400
close
EOF

rc=$(run "$WORK/tear.session")
if [ "$rc" = 0 ] && ! grep -qE "CRITICAL|SEGMENTATION|Segmentation" "$WORK/log"; then
  ok "torn-down: close mid-stream exits cleanly, the transfer dies quietly"
else
  bad "torn-down: exit=$rc or criticals in the log"
fi

if [ "$fails" -eq 0 ]; then
  echo "source gate: all checks passed"
  exit 0
fi
echo "source gate: $fails check(s) failed"
exit 1
