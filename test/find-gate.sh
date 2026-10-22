#!/usr/bin/env bash
#
# test/find-gate.sh — drives the whole find path and asserts on it.
#
# Four runs against test/fake-helper.py cover the host's own layers (the script
# steps, the two commands, and the reply shape the panel is written against), and
# two more against the real CEF helper cover the browser's half: a page with a
# known number of occurrences reports that number, match_case reaches Chromium
# rather than being decorated on the host side, a search that matches nothing
# answers once instead of twice, and the step — not the search — moves the page
# to the match, which is the reason no match rectangle is on the wire.
#
# It opens a real demo window per run, so it is a documented manual gate rather
# than a meson test — the same convention as test/*.session and
# test/download-gate.sh. Nothing here moves a pointer on the host desktop.
#
#   ./test/find-gate.sh            # fake + real
#   ./test/find-gate.sh fake       # only the half that needs no browser
#   ./test/find-gate.sh real       # only the CEF half
#
# The real runs deliberately do not pin which match the first step lands on: seen
# once (1 run in 3), the first step after a new search reported ordinal 3 rather
# than 1, and docs/features/find-path.md keeps that as a measured hazard instead of a
# number to assert.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"
. "$ROOT/test/helper-path.sh"
PAGE="file://$ROOT/test/find-page.html"
WHICH=${1:-all}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -n 'find' "$2" | head -8 | sed 's/^/         /'
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

nth_scroll() { # nth_scroll <logfile> <n> — the n-th scrollTop a js step read back
  sed -n 's/^demo: script [0-9]* ok value=\([0-9.]*\).*/\1/p' "$1" | sed -n "$2p"
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
# The fake helper: the host's own layers, with the reply shape under our control.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = fake ]; then
  if [ ! -f "$FAKE" ]; then echo "no fake helper at $FAKE"; exit 1; fi

  # 1. Counts, ordinals and the counting-then-final shape. The fake page is
  #    one fixed sentence, so six is six.
  script fake-counts <<EOF
load http://gate.test/page
wait 700
find e
wait 250
find-next
wait 250
find-next
wait 250
find-prev
wait 250
find zzz
wait 250
close
EOF
  run_demo fake-counts "$WORK/fake-counts.log" "$FAKE"
  expect 'find pattern=e forward=1 match_case=0 next=0' "$WORK/fake-counts.log" fake-counts
  expect 'find result id=1 count=6 ordinal=0 final=1' "$WORK/fake-counts.log" fake-counts
  expect 'find result id=2 count=6 ordinal=1 final=1' "$WORK/fake-counts.log" fake-counts
  expect 'find result id=3 count=6 ordinal=2 final=1' "$WORK/fake-counts.log" fake-counts
  expect 'find result id=4 count=6 ordinal=1 final=1' "$WORK/fake-counts.log" fake-counts
  expect 'find pattern=e forward=0 match_case=0 next=1' "$WORK/fake-counts.log" fake-counts
  # Four requests found something, so four counting updates — and no more.
  expect_count 'find result id=[0-9]+ count=6 ordinal=[0-9]+ final=0' \
    "$WORK/fake-counts.log" 4 fake-counts
  # A search that matches nothing answers once, and that one is the final.
  expect 'find result id=5 count=0 ordinal=0 final=1' "$WORK/fake-counts.log" fake-counts
  expect_count 'count=0 ordinal=0 final=0' "$WORK/fake-counts.log" 0 fake-counts

  # 2. match_case is a flag that has to reach the far end, not a host-side filter.
  script fake-case <<EOF
load http://gate.test/page
wait 700
find PAGE
wait 250
find-case PAGE
wait 250
find-case page
wait 250
close
EOF
  run_demo fake-case "$WORK/fake-case.log" "$FAKE"
  expect 'find pattern=PAGE forward=1 match_case=0 next=0' "$WORK/fake-case.log" fake-case
  expect 'find pattern=PAGE forward=1 match_case=1 next=0' "$WORK/fake-case.log" fake-case
  expect 'find result id=1 count=1 ordinal=0 final=1' "$WORK/fake-case.log" fake-case
  expect 'find result id=2 count=0 ordinal=0 final=1' "$WORK/fake-case.log" fake-case
  expect 'find result id=3 count=1 ordinal=0 final=1' "$WORK/fake-case.log" fake-case
  expect_count 'find result' "$WORK/fake-case.log" 5 fake-case

  # 3. Stopping is silent, and the row is over: nothing arrives between the stop
  #    and the next request.
  script fake-stop <<EOF
load http://gate.test/page
wait 700
find e
wait 250
find-stop
wait 250
find zzz
wait 250
close
EOF
  run_demo fake-stop "$WORK/fake-stop.log" "$FAKE"
  expect 'find stop clear=1' "$WORK/fake-stop.log" fake-stop
  # Two updates for the search, one for the no-match search, nothing for the stop.
  expect_count 'find result' "$WORK/fake-stop.log" 3 fake-stop
  expect 'find result id=2 count=0 ordinal=0 final=1' "$WORK/fake-stop.log" fake-stop

  # 4. A request can answer with more than one counting update. Measured on real
  #    Chromium: a 上一个 taken straight after a new search reports ordinal 0 (the
  #    search has no active match yet) and then wraps to the last match, three
  #    lines for one identifier. The host must render on final, not on "the
  #    second update".
  script fake-wrap <<EOF
load http://gate.test/page
wait 700
find e
wait 250
find-prev
wait 250
close
EOF
  run_demo fake-wrap "$WORK/fake-wrap.log" "$FAKE"
  expect 'find result id=1 count=6 ordinal=0 final=1' "$WORK/fake-wrap.log" fake-wrap
  expect 'find result id=2 count=6 ordinal=0 final=0' "$WORK/fake-wrap.log" fake-wrap
  expect 'find result id=2 count=6 ordinal=6 final=0' "$WORK/fake-wrap.log" fake-wrap
  expect 'find result id=2 count=6 ordinal=6 final=1' "$WORK/fake-wrap.log" fake-wrap
  expect_count 'find result id=2' "$WORK/fake-wrap.log" 3 fake-wrap
  expect_count 'find result' "$WORK/fake-wrap.log" 5 fake-wrap
fi

# ---------------------------------------------------------------------------
# The real helper: what Chromium counts, what it answers, and what it scrolls to.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = real ]; then
  if [ ! -x "$HELPER" ]; then echo "no helper at $HELPER — run cmake --build helper/build"; exit 1; fi

  # 5. The page has seven "mango" and one "Mango": eight for a folded search,
  #    seven for a case-sensitive one, and the gate asserts both numbers because
  #    the difference is the flag doing its work in the browser.
  script real-page <<EOF
load $PAGE
wait 2500
find mango
wait 1000
find-next
wait 700
find-next
wait 700
find-prev
wait 1000
find-case mango
wait 1000
find nomatch-xyz
wait 1000
find-stop
wait 400
close
EOF
  run_demo real-page "$WORK/real-page.log" "$HELPER"
  expect 'loaded .*find-page\.html \(HTTP 200\)' "$WORK/real-page.log" real-page
  # A new search reports no active match at all.
  expect 'find result id=[0-9]+ count=8 ordinal=0 final=1' "$WORK/real-page.log" real-page
  # Three step requests, each with an active match somewhere in the eight.
  expect_count 'find result id=[0-9]+ count=8 ordinal=[1-8] final=1' \
    "$WORK/real-page.log" 3 real-page
  expect 'find result id=[0-9]+ count=7 ordinal=0 final=1' "$WORK/real-page.log" real-page
  expect 'find result id=[0-9]+ count=0 ordinal=0 final=1' "$WORK/real-page.log" real-page
  expect_no 'count=0 ordinal=0 final=0' "$WORK/real-page.log" real-page
  expect 'find stop clear=1' "$WORK/real-page.log" real-page

  # 6. The only match is 3000 px down the page and nobody touched the wheel. The
  #    search itself leaves the page where it was — nothing is the active match
  #    yet — and the step that selects one scrolls it into view. Both halves are
  #    asserted because together they are the reason no match rectangle is on
  #    the wire: the browser moves the page, the host just reports counts.
  script real-scroll <<EOF
load $PAGE
wait 2500
find belowthefold
wait 1500
js document.scrollingElement.scrollTop
wait 900
find-next
wait 1800
js document.scrollingElement.scrollTop
wait 900
close
EOF
  run_demo real-scroll "$WORK/real-scroll.log" "$HELPER"
  expect 'find result id=[0-9]+ count=1 ordinal=0 final=1' "$WORK/real-scroll.log" real-scroll
  first=$(nth_scroll "$WORK/real-scroll.log" 1)
  second=$(nth_scroll "$WORK/real-scroll.log" 2)
  if [ -z "$first" ] || [ -z "$second" ]; then
    bad "real-scroll: could not read back both scrollTop values (got '${first:-none}', '${second:-none}')"
  else
    if awk "BEGIN { exit !($first < 50) }"; then
      ok "real-scroll: the search left the page alone (scrollTop $first)"
    else bad "real-scroll: scrollTop was $first after the search, wanted under 50"; fi
    if awk "BEGIN { exit !($second > 500) }"; then
      ok "real-scroll: the step brought the match into view (scrollTop $second)"
    else bad "real-scroll: scrollTop was $second after the step, wanted over 500"; fi
  fi
fi

echo
if [ $fails -eq 0 ]; then
  printf 'find gate: all checks passed\n'
else
  printf 'find gate: %d check(s) failed\n' "$fails"
  exit 1
fi
