#!/usr/bin/env bash
#
# test/permission-gate.sh — drives the permission-prompt path and asserts on it.
#
# Seven runs against test/fake-helper.py cover the host's own layers: the two
# answers, the four policies, and the shapes the queue rules make — a media ask that
# holds the asks made behind it until it is answered, a prompt that is taken back the
# moment the next one shows up, and Chromium closing a request the host never
# answered. Three runs against the real CEF helper cover the browser's half: what a
# page sees when every ask is denied, that an answer the host gave really reaches the
# page, and that the media queue is Chromium's and not the fake's.
#
# It opens a real demo window per run, so it is a documented manual gate rather
# than a meson test — the same convention as test/menu-gate.sh. Nothing here moves
# a pointer on the host desktop: every click is injected into the nested
# compositor. No camera or microphone is ever opened: every media ask in this file
# ends in a deny, and the accept that is asserted is a notification, which needs
# no device.
#
# Each real run gets its own XDG_CACHE_HOME so it starts on a profile nobody else
# decided on: state left behind by the run before it would be a decision this run
# asserts but never made. Measured, a permission the host denied is still asked of
# the host the next time (docs/permission-prompt.md §7), so this is isolation
# between runs rather than a defence against a remembered answer.
#
#   ./test/permission-gate.sh            # fake + real
#   ./test/permission-gate.sh fake       # only the half that needs no browser
#   ./test/permission-gate.sh real       # only the CEF half
#
# Every run pipes stderr through tee: GLib drops debug-level messages when stderr
# is a regular file, and the session's "host took it" and "dismissed" lines are the
# only proof of which half of the contract answered.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"
HELPER="$ROOT/helper/build/Release/cefnest-helper"
PAGE="file://$ROOT/test/permission-page.html"
WHICH=${1:-all}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -n 'permission' "$2" | head -8 | sed 's/^/         /'
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

# The queue rules are about order, not just presence: an ask that arrives because
# an answer freed the slot has to come after that answer, and two greps cannot say
# so. This compares the first line matching each pattern.
expect_after() { # expect_after <earlier> <later> <logfile> <tag>
  local a b
  a=$(grep -nE "$1" "$3" | head -1 | cut -d: -f1)
  b=$(grep -nE "$2" "$3" | head -1 | cut -d: -f1)
  if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ]; then
    ok "$4: /$2/ (line $b) came after /$1/ (line $a)"
  else
    bad "$4: wanted /$2/ after /$1/ — got '${b:-none}' and '${a:-none}'"
  fi
}

# The page reads its own outcomes back through a js step, which reports them as one
# quoted string; pulling the n-th one out keeps an assertion about what a page saw
# honest rather than about the order the trace happened in.
js_value() { # js_value <logfile> <n> — the n-th value a js step reported
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
}

expect_outcome() { # expect_outcome <logfile> <n> <ask:state> <tag>
  local value
  value=$(js_value "$1" "$2")
  case "$value" in
    *"$3"*) ok "$4: the page saw $3" ;;
    *) bad "$4: wanted /$3/ in '${value:-none}'" ;;
  esac
}

run_demo() { # run_demo <tag> <logfile> <helper> <extra env...>
  local tag=$1 log=$2 helper=$3
  shift 3

  if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
  if [ ! -e "$helper" ]; then echo "no helper at $helper"; exit 1; fi
  echo "== $tag"
  timeout 90 env "$@" G_MESSAGES_DEBUG=all GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$helper" CHROMIUM_SESSION_SCRIPT="$WORK/$tag.session" \
    "$DEMO" 2>&1 | tee "$log" | cat > /dev/null
  local status=${PIPESTATUS[0]}
  if [ $status -ne 0 ]; then
    bad "$tag: the demo exited with $status"
    tail -6 "$log" | sed 's/^/         /'
  fi
}

script() { # script <tag> writes the heredoc that follows into the tag's script
  cat > "$WORK/$1.session"
}

# ---------------------------------------------------------------------------
# The fake helper: the host's own layers, with the ask under our control.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = fake ]; then
  # 1. A prompt asked, shown, and answered twice over — once by a step and once by
  #    the same step against a row that is already gone.
  script fake-accept <<EOF
load http://gate.test/permission?perm=geolocation
wait 700
perm-accept 1
wait 400
perm-deny 1
wait 400
load http://gate.test/permission?perm=notifications
wait 700
perm-deny 2
wait 400
close
EOF
  run_demo fake-accept "$WORK/fake-accept.log" "$FAKE" CEFNEST_PERMISSION=ask
  expect 'demo: permissions: policy=ask' "$WORK/fake-accept.log" fake-accept
  expect 'demo: permission 1 kind=prompt names=geolocation raw=256' \
    "$WORK/fake-accept.log" fake-accept
  expect 'session: permission 1 \(prompt\) from .* host took it: 1' \
    "$WORK/fake-accept.log" fake-accept
  expect 'fake: permission-respond id=1 result=accept kind=prompt names=geolocation' \
    "$WORK/fake-accept.log" fake-accept
  # The row is decided, so the second step cannot cross the wire twice — and the
  # session is the half that says why.
  expect 'demo: permission 1 was already answered' "$WORK/fake-accept.log" fake-accept
  expect_count 'fake: permission-respond id=1' "$WORK/fake-accept.log" 1 fake-accept
  expect 'demo: permission 2 kind=prompt names=notifications raw=32768' \
    "$WORK/fake-accept.log" fake-accept
  expect 'fake: permission-respond id=2 result=deny' "$WORK/fake-accept.log" fake-accept

  # 2. Two prompts with no answer between them: Chromium takes the held one back
  #    the moment the next arrives, and the host has to put its own row down.
  script fake-yank <<EOF
load http://gate.test/permission?perm=geolocation
wait 700
load http://gate.test/permission?perm=clipboard
wait 700
perm-accept 1
wait 400
perm-dismiss 2
wait 400
close
EOF
  run_demo fake-yank "$WORK/fake-yank.log" "$FAKE" CEFNEST_PERMISSION=ask
  expect 'session: permission 1 dismissed as ignore' "$WORK/fake-yank.log" fake-yank
  expect 'demo: permission 1 dismissed as ignore' "$WORK/fake-yank.log" fake-yank
  expect 'demo: permission 2 kind=prompt names=clipboard raw=16' \
    "$WORK/fake-yank.log" fake-yank
  # The dead row answers nothing, and the dismiss that follows is the live one.
  expect 'demo: permission 1 was already answered' "$WORK/fake-yank.log" fake-yank
  expect_count 'fake: permission-respond' "$WORK/fake-yank.log" 1 fake-yank
  expect 'fake: permission-respond id=2 result=dismiss kind=prompt names=clipboard' \
    "$WORK/fake-yank.log" fake-yank

  # 3. The media queue: an unanswered ask holds the asks behind it, and answering it
  #    is what lets the next one through.
  script fake-media <<EOF
load http://gate.test/permission?perm=audio
wait 700
load http://gate.test/permission?perm=video
wait 700
perm-deny 1
wait 700
perm-deny 2
wait 400
close
EOF
  run_demo fake-media "$WORK/fake-media.log" "$FAKE" CEFNEST_PERMISSION=ask
  expect 'demo: permission 1 kind=media names=device-audio raw=1' \
    "$WORK/fake-media.log" fake-media
  # The second ask crossed the wire while the first was still held, and nothing
  # arrived for it until the answer freed the slot.
  expect_count 'demo: permission [0-9]+ kind=' "$WORK/fake-media.log" 2 fake-media
  expect_after 'fake: permission-respond id=1' 'demo: permission 2 kind=media' \
    "$WORK/fake-media.log" fake-media
  expect 'fake: permission-respond id=1 result=deny kind=media names=device-audio' \
    "$WORK/fake-media.log" fake-media
  expect 'demo: permission 2 kind=media names=device-video raw=2' \
    "$WORK/fake-media.log" fake-media
  expect 'fake: permission-respond id=2 result=deny kind=media names=device-video' \
    "$WORK/fake-media.log" fake-media

  # 4. deny on arrival: the row stays as the record, and nothing waits for a click.
  script fake-deny <<EOF
load http://gate.test/permission?perm=storage
wait 700
perm-accept 1
wait 400
close
EOF
  run_demo fake-deny "$WORK/fake-deny.log" "$FAKE" CEFNEST_PERMISSION=deny
  expect 'demo: permissions: policy=deny' "$WORK/fake-deny.log" fake-deny
  expect 'demo: permission 1 deny' "$WORK/fake-deny.log" fake-deny
  expect 'fake: permission-respond id=1 result=deny kind=prompt names=storage-access' \
    "$WORK/fake-deny.log" fake-deny
  # Already answered, so the step that follows crosses nothing.
  expect_count 'fake: permission-respond' "$WORK/fake-deny.log" 1 fake-deny

  # 5. allow on arrival, and an ask carrying two permission bits at once.
  script fake-allow <<EOF
load http://gate.test/permission?perm=network
wait 700
close
EOF
  run_demo fake-allow "$WORK/fake-allow.log" "$FAKE" CEFNEST_PERMISSION=allow
  expect 'demo: permission 1 kind=prompt names=local-network\+loopback-network raw=201326592' \
    "$WORK/fake-allow.log" fake-allow
  expect 'demo: permission 1 accept' "$WORK/fake-allow.log" fake-allow
  expect 'fake: permission-respond id=1 result=accept' "$WORK/fake-allow.log" fake-allow

  # 6. A host that does not take the question: the session answers it, and the only
  #    answer it has is a deny.
  script fake-ignore <<EOF
load http://gate.test/permission?perm=audio
wait 700
close
EOF
  run_demo fake-ignore "$WORK/fake-ignore.log" "$FAKE" CEFNEST_PERMISSION=ignore
  expect 'demo: permission 1 kind=media .* policy=ignore' "$WORK/fake-ignore.log" fake-ignore
  expect 'session: permission 1 \(media\) from .* host took it: 0' \
    "$WORK/fake-ignore.log" fake-ignore
  expect 'fake: permission-respond id=1 result=deny kind=media names=device-audio' \
    "$WORK/fake-ignore.log" fake-ignore
  # The panel never decided this one, so it never says it did.
  expect_no 'demo: permission 1 deny' "$WORK/fake-ignore.log" fake-ignore

  # 7. The helper closing a request the host never answered, and an ask name the
  #    fake has no model for.
  script fake-auto <<EOF
load http://gate.test/permission?perm=midi&auto=1
wait 1200
perm-accept 1
wait 400
load http://gate.test/permission?perm=vibration
wait 700
close
EOF
  run_demo fake-auto "$WORK/fake-auto.log" "$FAKE" CEFNEST_PERMISSION=ask
  expect 'demo: permission 1 kind=prompt names=midi-sysex raw=8192' \
    "$WORK/fake-auto.log" fake-auto
  expect 'session: permission 1 dismissed as ignore' "$WORK/fake-auto.log" fake-auto
  expect 'demo: permission 1 was already answered' "$WORK/fake-auto.log" fake-auto
  expect_count 'fake: permission-respond' "$WORK/fake-auto.log" 0 fake-auto
  expect 'helper status: unknown-permission' "$WORK/fake-auto.log" fake-auto
fi

# ---------------------------------------------------------------------------
# The real helper: what Chromium does with the answers, and what a page sees.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = real ]; then
  # 8. Every ask denied, which is what a host with no permission UI gives a page.
  #    The ten outcomes below are the contract: a denied media ask rejects the way
  #    it did before any handler existed, a denied prompt rejects rather than hangs,
  #    and the three asks that never reach the handler keep the answers Chromium
  #    gives them on its own.
  script real-deny <<EOF
load $PAGE
wait 3000
js window.context()
wait 300
click 170 42
wait 500
click 170 102
wait 500
click 170 162
wait 500
click 170 222
wait 500
click 170 282
wait 500
click 170 342
wait 500
click 170 402
wait 500
click 170 462
wait 500
click 490 42
wait 500
click 490 102
wait 2500
js window.outcome()
wait 1000
close
EOF
  run_demo real-deny "$WORK/real-deny.log" "$HELPER" \
    CEFNEST_PERMISSION=ignore XDG_CACHE_HOME="$WORK/profile-deny"
  expect 'loaded .*permission-page\.html \(HTTP 200\)' "$WORK/real-deny.log" real-deny
  expect_count 'helper: permission .* answered deny' "$WORK/real-deny.log" 7 real-deny
  expect 'demo: permission 1 kind=media names=device-audio raw=1' \
    "$WORK/real-deny.log" real-deny
  expect 'demo: permission 4 kind=prompt names=geolocation raw=256' \
    "$WORK/real-deny.log" real-deny
  expect 'demo: permission 7 kind=prompt names=midi-sysex raw=8192' \
    "$WORK/real-deny.log" real-deny
  expect_outcome "$WORK/real-deny.log" 2 \
    'audio:NotAllowedError|video:NotAllowedError|both:NotAllowedError|geo:code1|notify:denied|clipboard:NotAllowedError|persist:false|query-camera:prompt|midi:NotAllowedError|idle:no-idle-api' \
    real-deny

  # 9. The two answers a host can give, each on a request of its own, read back
  #    through the page. Denying geolocation is Chromium's PERMISSION_DENIED rather
  #    than the POSITION_UNAVAILABLE an unanswered ask becomes; accepting a
  #    notification is the one grant that needs no device behind it.
  script real-answer <<EOF
load $PAGE
wait 3000
click 170 222
wait 700
perm-deny 1
wait 700
click 170 282
wait 700
perm-accept 2
wait 1500
js window.outcome()
wait 1000
close
EOF
  run_demo real-answer "$WORK/real-answer.log" "$HELPER" \
    CEFNEST_PERMISSION=ask XDG_CACHE_HOME="$WORK/profile-answer"
  expect 'demo: permission 1 kind=prompt names=geolocation' \
    "$WORK/real-answer.log" real-answer
  expect 'demo: permission 2 kind=prompt names=notifications' \
    "$WORK/real-answer.log" real-answer
  expect 'helper: permission 1 answered deny' "$WORK/real-answer.log" real-answer
  expect 'helper: permission 2 answered accept' "$WORK/real-answer.log" real-answer
  expect_outcome "$WORK/real-answer.log" 1 'geo:code1' real-answer
  expect_outcome "$WORK/real-answer.log" 1 'notify:granted' real-answer

  # 10. A media request the panel is holding: the queue is Chromium's, not the
  #     fake's. The ask made behind a held request arrives as its own request only
  #     once the answer has gone back — and the page, which has been waiting the
  #     whole time, is the one that says so.
  script real-park <<EOF
load $PAGE
wait 3000
click 170 42
wait 900
click 170 102
wait 900
js window.outcome()
wait 400
perm-deny 1
wait 900
js window.outcome()
wait 400
click 170 162
wait 900
perm-deny 2
wait 900
js window.outcome()
wait 400
perm-deny 3
wait 1200
js window.outcome()
wait 1000
close
EOF
  run_demo real-park "$WORK/real-park.log" "$HELPER" \
    CEFNEST_PERMISSION=ask XDG_CACHE_HOME="$WORK/profile-park"
  expect 'demo: permission 1 kind=media names=device-audio' \
    "$WORK/real-park.log" real-park
  # Three asks, three requests, and no second request before the first answer.
  expect_count 'demo: permission [0-9]+ kind=media' "$WORK/real-park.log" 3 real-park
  expect_after 'demo: permission 1 kind=media' 'helper: permission 1 answered deny' \
    "$WORK/real-park.log" real-park
  expect_after 'helper: permission 1 answered deny' 'demo: permission 2 kind=media' \
    "$WORK/real-park.log" real-park
  expect 'demo: permission 3 kind=media names=device-audio\+device-video raw=3' \
    "$WORK/real-park.log" real-park
  # Held: nothing settled. Denied: each one rejected. That is the queue, read from
  # the page rather than from the trace.
  expect_outcome "$WORK/real-park.log" 1 'audio:pending|video:pending' real-park
  expect_outcome "$WORK/real-park.log" 2 'audio:NotAllowedError|video:pending' real-park
  expect_outcome "$WORK/real-park.log" 3 'audio:NotAllowedError|video:NotAllowedError|both:pending' \
    real-park
  expect_outcome "$WORK/real-park.log" 4 \
    'audio:NotAllowedError|video:NotAllowedError|both:NotAllowedError' real-park
  # Nothing was ever granted, so no capture device was opened.
  expect_no 'answered accept' "$WORK/real-park.log" real-park
fi

echo
if [ $fails -eq 0 ]; then
  printf 'permission gate: all checks passed\n'
else
  printf 'permission gate: %d check(s) failed\n' "$fails"
  exit 1
fi
