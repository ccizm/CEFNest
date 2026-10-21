#!/usr/bin/env bash
#
# test/ci-all.sh — the whole unattended battery in one command.
#
# It runs every gate test/ci-session.sh can host, one after another, and prints a
# verdict table. Nothing here opens a window on the desktop of whoever is sitting
# at this machine, so the run can be left going: that is what makes it a release
# gate rather than the manual gates README documents one by one.
#
#   ./test/ci-all.sh                 # everything: measured 367 s on this laptop
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
# skips exactly what this display cannot prove (docs/headless-ci.md §5).

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1          # the payloads below are paths relative to the repo
KEEP_LOGS=${KEEP_LOGS:-0}

# label|payload|timeout — the payload runs inside the nested session, so a path
# is relative to the repo root, which ci-session.sh inherits as its cwd.
ALL_RUNS=(
  "proof||240"
  "unit|meson test -C builddir|240"
  "pixel|./test/pixel-gate.sh|240"
  "find|./test/find-gate.sh|420"
  "permission|./test/permission-gate.sh|420"
  "download|./test/download-gate.sh|420"
  "dnd|./test/dnd-gate.sh|420"
  "menu|./test/menu-gate.sh|420"
  "soak|./test/sandbox-soak.sh|600"
)

LOGS=$(mktemp -d)
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
for spec in "${ALL_RUNS[@]}"; do
  label=${spec%%|*}
  rest=${spec#*|}
  payload=${rest%%|*}
  tmo=${rest#*|}
  wanted "$label" || continue
  log=$LOGS/$label.log
  t0=$(date +%s)
  if [ -z "$payload" ]; then
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
  if [ $rc = 0 ]; then verdict=PASS
  else verdict=FAIL; fails=$((fails + 1)); fi
  [ "$skipped" = 0 ] || verdict="$verdict*"
  printf '%-10s %-6s %5ss %6s %6s\n' "$label" "$verdict" "$(( $(date +%s) - t0 ))" \
    "$assertions" "$skipped"
  if [ $rc != 0 ]; then
    tail -25 "$log" | sed 's/^/         /'
  elif [ "$KEEP_LOGS" != 1 ]; then
    rm -f "$log"
  fi
done

if [ "$KEEP_LOGS" != 1 ]; then
  rmdir "$LOGS" 2>/dev/null || echo "  note: a failed run left its transcript in $LOGS"
fi

echo
echo "(* = passed with skipped checks: those are the ones a display without an"
echo "    output device cannot answer, docs/headless-ci.md §5)"
if [ $fails = 0 ]; then printf 'ci all: every run passed\n'
else printf 'ci all: %d run(s) failed — transcripts above\n' "$fails"; exit 1; fi
