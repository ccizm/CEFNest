#!/usr/bin/env bash
#
# test/pdf-gate.sh — PrintToPDF, streamed back as bytes and not as a path.
#
# CEF's PrintToPDF writes a file; the helper prints into its own ephemeral
# directory, reads the file back, streams it in base64 chunks and removes it.
# This gate runs that twice:
#
#   full       the whole PDF: %PDF- at the head, %%EOF at the tail, one
#              pdf-begin/end pair, and the byte count equals the file on disk
#   truncated  a 2000-byte cap: pdf-begin says truncated, exactly 2000 bytes
#              arrive, and the file is cut — the cap a host or a test uses to
#              prove the stop path
#
# The demo writes what it received to $CEFNEST_PDF_OUT, so the gate grades the
# host-visible bytes, not a file the helper kept behind.
#
#   ./test/pdf-gate.sh
#   ./test/ci-session.sh ./test/pdf-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/browser-data-page.html"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'pdf|loaded|helper: print' "$2" | tail -10 | sed 's/^/         /'
  fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi

# run_pdf <tag> <step> <out.pdf> — one demo run, one print-pdf, one file.
run_pdf() {
  local tag=$1 step=$2 out=$3 log=$WORK/$1.log

  cat >"$WORK/$tag.session" <<EOF
load $PAGE
wait 2000
$step
wait 4500
close
EOF
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland G_MESSAGES_DEBUG=all \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$WORK/$tag.session" \
    CEFNEST_PDF_OUT="$out" \
    "$DEMO" >"$log" 2>&1
  echo $?
}

echo "== full PDF"
status=$(run_pdf full print-pdf "$WORK/full.pdf")
log=$WORK/full.log
if [ "$status" = 0 ]; then ok "full: demo exited 0"
else bad "full: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: loaded file://.*browser-data-page\.html \(HTTP 200\)' "$log" page
expect 'demo: pdf-begin id=[0-9]+ bytes=[1-9][0-9]* truncated=0' "$log" begin
expect 'demo: pdf id=[0-9]+ bytes=[1-9][0-9]* truncated=0 error="" magic=1 eof=1' \
  "$log" answer

if [ -f "$WORK/full.pdf" ] && [ "$(head -c 5 "$WORK/full.pdf")" = "%PDF-" ]; then
  ok "full: the bytes on disk start with %PDF-"
else
  bad "full: no PDF at $WORK/full.pdf, or the wrong magic"
fi
if [ -f "$WORK/full.pdf" ] && tail -c 1024 "$WORK/full.pdf" | grep -aq '%%EOF'; then
  ok "full: %%EOF in the last 1 KiB"
else
  bad "full: the file has no %%EOF near its end"
fi

size=$(stat -c %s "$WORK/full.pdf" 2>/dev/null || echo 0)
reported=$(sed -n 's/^demo: pdf id=[0-9]* bytes=\([0-9]*\) .*/\1/p' "$log" | head -1)
if [ "$size" = "$reported" ] && [ "$size" -gt 0 ]; then
  ok "full: $size bytes received, the same on disk"
else
  bad "full: demo reported '$reported' bytes, the file holds '$size'"
fi

echo "== truncated at 2000 bytes"
status=$(run_pdf cut "print-pdf 2000" "$WORK/cut.pdf")
log=$WORK/cut.log
if [ "$status" = 0 ]; then ok "cut: demo exited 0"
else bad "cut: demo exited $status"; tail -8 "$log" | sed 's/^/         /'; fi

expect 'demo: pdf-begin id=[0-9]+ bytes=2000 truncated=1' "$log" begin
expect 'demo: pdf id=[0-9]+ bytes=2000 truncated=1 error="" magic=1' "$log" answer
if [ "$(stat -c %s "$WORK/cut.pdf" 2>/dev/null || echo 0)" = 2000 ]; then
  ok "cut: exactly 2000 bytes reached the host"
else
  bad "cut: the host did not receive 2000 bytes"
fi

echo
if [ $fails = 0 ]; then printf 'pdf gate: all checks passed\n'
else printf 'pdf gate: %d check(s) failed\n' "$fails"; exit 1; fi
