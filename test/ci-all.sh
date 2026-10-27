#!/usr/bin/env bash
#
# test/ci-all.sh — the whole unattended battery in one command.
#
# It runs every gate test/ci-session.sh can host, one after another, and prints a
# verdict table. Nothing here opens a window on the desktop of whoever is sitting
# at this machine, so the run can be left going: that is what makes it a release
# gate rather than the manual gates that have to be run one by one.
#
#   ./test/ci-all.sh                 # everything: measured 370 s on this laptop
#   ./test/ci-all.sh find menu       # only the named ones
#   KEEP_LOGS=1 ./test/ci-all.sh     # keep each run's full transcript
#
# Each run gets its own nested compositor even though one could host them all
# (`./test/ci-session.sh bash -c './test/find-gate.sh && ./test/menu-gate.sh'` is a
# dozen seconds cheaper, and measured green). A run that hangs, aborts the browser
# process or leaves its compositor in a bad state then costs only its own line in
# the table — with everything in one session it would take the verdicts of the
# gates after it with it, and a gate battery that can only report "something in
# there failed" is not a gate.
#
# `skip` is counted and shown because a green line must never hide one: a gate
# skips exactly what this display cannot prove. A gate
# that could not run AT ALL — no ImageMagick, no browser build — exits 77 and gets
# its own SKIP verdict instead of a PASS: the one thing worse than a hidden skip is
# a verdict of "passed" from a run that made no assertion, because an exit code is
# what everything above this table (a shell `&&`, a CI step) actually reads.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1          # the payloads below are paths relative to the repo
KEEP_LOGS=${KEEP_LOGS:-0}

# The helper is built by cmake, not meson, so `meson test` below never sees a
# change under helper/src. Every real-CEF gate runs the binary this step
# produces; without it a green table only says the last manual build still
# works. Build it here, as part of the gate, unless the caller is explicit
# about grading something else (CI_SKIP_HELPER_BUILD=1).
if [ "${CI_SKIP_HELPER_BUILD:-0}" != 1 ]; then
  # third_party/ is not in git: a runner can keep an old CEF distribution
  # forever. Refuse to grade a helper built against something other than the
  # version fetch-cef.sh recorded for this tree.
  echo "== cef pin"
  if ! bash "$ROOT/scripts/check-cef-pin.sh"; then
    echo "ci all: the CEF distribution does not match the version recorded by fetch-cef.sh" >&2
    exit 1
  fi
  HELPER_BUILD=${HELPER_BUILD:-"$ROOT/helper/build"}
  if [ ! -f "$HELPER_BUILD/CMakeCache.txt" ]; then
    echo "ci all: no helper build tree at $HELPER_BUILD" >&2
    echo "  run: cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release" >&2
    exit 1
  fi
  echo "== helper build"
  if ! cmake --build "$HELPER_BUILD" --config Release \
       -j"$(nproc 2>/dev/null || echo 2)"; then
    echo "ci all: helper build failed" >&2
    exit 1
  fi
fi

# label|payload|timeout — the payload runs inside the nested session, so a path
# is relative to the repo root, which ci-session.sh inherits as its cwd.
ALL_RUNS=(
  # Each timeout is the gate's own per-run budget times its number of runs,
  # plus a margin: menu 8x90, permission 10x90, download 11x90, find/dnd 6x90,
  # js-dialog 4x90, file-dialog 3x90. A budget smaller than that loses both the
  # verdict and the transcript of whichever run hung.
  "proof||240"
  "unit|meson test -C builddir|240"
  "framing|./test/framing-gate.sh|420"
  "proxy|./test/proxy-gate.sh|240"
  "accept-language|./test/accept-language-gate.sh|180"
  "autoplay|./test/autoplay-gate.sh|240"
  "console|./test/console-gate.sh|240"
  "print|./test/print-gate.sh|240"
  "hang|./test/hang-gate.sh|240"
  "frame|./test/frame-gate.sh|300"
  "zoom|./test/zoom-gate.sh|180"
  "auto-resize|./test/auto-resize-gate.sh|180"
  "cookies|./test/cookies-gate.sh|240"
  "page-text|./test/page-text-gate.sh|180"
  "pdf|./test/pdf-gate.sh|300"
  "favicon|./test/favicon-gate.sh|180"
  "source|./test/source-gate.sh|240"
  "pixel|./test/pixel-gate.sh|240"
  "find|./test/find-gate.sh|600"
  "permission|./test/permission-gate.sh|960"
  "download|./test/download-gate.sh|1050"
  "dnd|./test/dnd-gate.sh|600"
  "menu|./test/menu-gate.sh|780"
  "devtools|./test/devtools-gate.sh|600"
  "certificate|./test/certificate-gate.sh|300"
  "auth|./test/auth-gate.sh|420"
  "tls-info|./test/tls-info-gate.sh|300"
  "intercept|./test/intercept-gate.sh|300"
  "filter|./test/filter-gate.sh|540"
  "cdp|./test/cdp-gate.sh|300"
  "history|./test/history-gate.sh|420"
  "browsers|./test/browsers-gate.sh|300"
  "window|./test/window-gate.sh|240"
  "scheme|./test/scheme-gate.sh|420"
  "script|./test/script-gate.sh|420"
  "ua|./test/ua-gate.sh|300"
  "screenshot|./test/screenshot-gate.sh|240"
  "clear-data|./test/clear-data-gate.sh|360"
  "js-dialog|./test/js-dialog-gate.sh|600"
  "file-dialog|./test/file-dialog-gate.sh|480"
  "page-events|./test/page-events-gate.sh|300"
  "plugin|./test/plugin-gate.sh|600"
  "surface-pair|./test/surface-pair-gate.sh|600"
  "primary|./test/primary-gate.sh|240"
  "multi|./test/multi-browser-gate.sh|420"
  "datadir|./test/data-dir-gate.sh|420"
  "signal|./test/helper-signal-gate.sh|240"
  "soak|./test/sandbox-soak.sh|600"
)
# Real sites are external and change under us, so they are opt-in. The batch
# starts one ci-session per site itself; wrapping it in another ci-session would
# nest two headless mutters for no gain, hence the special case in the loop.
if [ "${CI_ALL_REAL:-0}" = 1 ]; then
  ALL_RUNS+=("real|./test/real-sites.sh|1800")
fi

if [ -n "${CI_LOG_DIR:-}" ]; then
  # CI keeps the verdict table and every run's transcript as build artifacts.
  # A caller-named directory wins over mktemp, and keeping it is not optional:
  # the run is about to upload it.
  LOGS=$CI_LOG_DIR
  mkdir -p "$LOGS" || exit 1
  KEEP_LOGS=1
else
  LOGS=$(mktemp -d)
fi
only=("$@")

wanted() { # is this label selected?
  [ ${#only[@]} -eq 0 ] && return 0
  local want=$1 o
  for o in "${only[@]}"; do [ "$o" = "$want" ] && return 0; done
  return 1
}

echo "== logs: $LOGS"
printf '%-10s %-6s %6s %6s %6s\n' run result time ok skip
fails=0
gate_skips=0
for spec in "${ALL_RUNS[@]}"; do
  label=${spec%%|*}
  rest=${spec#*|}
  payload=${rest%%|*}
  tmo=${rest#*|}
  wanted "$label" || continue
  log=$LOGS/$label.log
  t0=$(date +%s)
  if [ "$label" = real ]; then
    timeout --kill-after=10s "$tmo" "$payload" >"$log" 2>&1
  elif [ -z "$payload" ]; then
    PAYLOAD_TIMEOUT=$tmo ./test/ci-session.sh >"$log" 2>&1
  else
    PAYLOAD_TIMEOUT=$tmo ./test/ci-session.sh bash -c "$payload" >"$log" 2>&1
  fi
  rc=$?
  # The harness reports around the payload with the same `  ok  ` prefix the gates
  # use, so its bookkeeping lines are subtracted; the proof mode's `transcript has`
  # lines stay in the count, because asserting on the transcript is all that mode does.
  total=$(grep -c '^  ok  ' "$log")
  harness=$(grep -cE '^  ok   (compositor socket|payload exited|demo exited|nothing from|[0-9]+ abort line)' "$log")
  assertions=$((total - harness))
  # `meson test` counts its results its own way; take its number rather than
  # reporting a run of thirteen tests as zero assertions.
  if [ "$assertions" = 0 ]; then
    m=$(sed -n 's/^Ok:[ ]*\([0-9]*\).*/\1/p' "$log" | tail -1)
    [ -n "$m" ] && assertions=$m
  fi
  skipped=$(grep -c '^  skip ' "$log")
  # 77 is meson's own "test skipped" status, so the two harnesses agree on what a
  # gate that ran nothing says.
  zero=""
  if [ $rc = 77 ]; then verdict=SKIP; gate_skips=$((gate_skips + 1))
  elif [ $rc = 0 ]; then
    # An exit status of 0 is not a pass when the gate asserted nothing: that is
    # exactly the early-`exit 0` shape the top of this file calls worse than a
    # hidden skip. A gate that legitimately ran no assertion exits 77.
    if [ "$assertions" = 0 ] && [ "$skipped" = 0 ]; then
      verdict=FAIL; fails=$((fails + 1)); zero=1
    else
      verdict=PASS
    fi
  else verdict=FAIL; fails=$((fails + 1)); fi
  # The star means "passed, but some checks were skipped". A gate that skipped
  # as a whole already says SKIP; adding the star made it look like a pass.
  if [ "$verdict" != "SKIP" ] && [ "$skipped" != 0 ]; then verdict="$verdict*"; fi
  printf '%-10s %-6s %5ss %6s %6s\n' "$label" "$verdict" "$(( $(date +%s) - t0 ))" \
    "$assertions" "$skipped"
  if [ -n "$zero" ]; then
    printf '         %s\n' "no assertion was made: status 0 is not a pass"
    tail -25 "$log" | sed 's/^/         /'
  elif [ $rc != 0 ]; then
    tail -25 "$log" | sed 's/^/         /'
  elif [ "$KEEP_LOGS" != 1 ]; then
    rm -f "$log"
  fi
done

if [ "$KEEP_LOGS" != 1 ]; then
  rmdir "$LOGS" 2>/dev/null || echo "  note: a failed run left its transcript in $LOGS"
fi

# A demo the harness lost (a gate's timeout firing without its kill-after, an
# orphaned nested session) does not just waste CPU: it keeps answering the
# very surface-commit paths the pixel gates read, and the next run inherits
# its numbers as its own. Name it so nobody has to find it in htop.
leftovers=$(pgrep -f cefnest-demo 2>/dev/null || true)
if [ -n "$leftovers" ]; then
  echo "ci all: WARNING: cefnest-demo still running after the battery — this run's pixel/soak numbers are not trustworthy:"
  echo "$leftovers" | sed 's/^/  pid /'
fi

echo
echo "(* = passed with skipped checks: those are the ones a display without an"
echo "    output device cannot answer)"
[ $gate_skips = 0 ] || echo "SKIP  = the gate ran no assertion here at all; the reason is in its transcript above"
if [ $fails = 0 ] && [ $gate_skips = 0 ]; then printf 'ci all: every run passed\n'
elif [ $fails = 0 ]; then printf 'ci all: every run that ran passed, %d skipped it\n' "$gate_skips"
else printf 'ci all: %d run(s) failed — transcripts above\n' "$fails"; exit 1; fi
