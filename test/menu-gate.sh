#!/usr/bin/env bash
#
# test/menu-gate.sh — drives the context-menu path and asserts on it.
#
# Six runs against test/fake-helper.py cover the host's own layers: the model as it
# arrives on the channel, what the session turns it into (sections, mnemonics, the
# types a check box would be), the two answers, and the three ways a request can go
# wrong around a host — an empty model, the helper closing the request itself, and a
# second menu before the first was answered. Two more against the real CEF helper
# cover the browser's half: the models Chromium builds for a link, an image, an
# editable and a selection, that a picked command runs Chromium's own action rather
# than being reported to anyone, and that a navigation with a menu open ends the
# request instead of eating every later right click.
#
# It opens a real demo window per run, so it is a documented manual gate rather than
# a meson test — the same convention as test/download-gate.sh. Nothing here moves a
# pointer on the host desktop: a right click is injected into the nested
# compositor, and the menu it opens is the host's own widget. It also runs inside
# ./test/ci-session.sh, where everything but the two popup-mapping assertions
# passes unchanged.
#
#   ./test/menu-gate.sh            # fake + real
#   ./test/menu-gate.sh fake       # only the half that needs no browser
#   ./test/menu-gate.sh real       # only the CEF half
#   ./test/ci-session.sh ./test/menu-gate.sh fake   # nobody sees the windows
#
# Every run pipes both streams through tee, because the session's debug lines come
# out on stdout once the process is off a tty: a popover on Wayland is its own
# surface, so no grab of the view can show it — "mapped=" and "drawn" are how a run
# knows the menu reached the screen. Those lines are why the runs need
# G_MESSAGES_DEBUG=all, and under test/ci-session.sh one of them cannot be
# satisfied: a display with no monitor never maps a popup surface, so
# expect_mapped() names that as a skip there and keeps it a hard assertion
# everywhere a screen exists.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
FAKE="$ROOT/test/fake-helper.py"
HELPER="$ROOT/helper/build/Release/cefnest-helper"
PAGE="file://$ROOT/test/menu-page.html"
OTHER="file://$ROOT/test/find-page.html"
WHICH=${1:-all}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0
skips=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -n 'menu' "$2" | head -8 | sed 's/^/         /'
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

# Whether a popup surface gets mapped is a question about the display, not about
# the menu: the build, the layout request and the size all happen on a monitor-less
# nested compositor, but the surface it produced is never put on a screen. Measured
# there: `mapped=0 size=0x0`, forever. Failing that would blame the run for the
# display, so it is reported as a skip — and every gate that reports a skip says
# how many, so a green line can never hide one.
expect_mapped() { # expect_mapped <logfile> <tag>
  if [ "${CEFNEST_HEADLESS:-0}" = 1 ]; then
    skips=$((skips + 1))
    printf '  skip %s: a popup surface is never mapped without a monitor\n' "$2"
  else
    expect 'context menu mapped=1 size=[1-9][0-9]*x[1-9][0-9]*' "$1" "$2"
  fi
}

# The page reads its own state back through a js step, which reports the value as
# one quoted string; pulling the n-th one out keeps the assertions about what a
# picked command did honest rather than about the order the trace happened in.
js_value() { # js_value <logfile> <n> — the n-th value a js step reported
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1" | sed -n "$2p"
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
# The fake helper: the host's own layers, with the model under our control.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = fake ]; then
  # 1. The link model, row for row, and the answer that goes back for it.
  script fake-link <<EOF
load http://gate.test/menu?menu=link&x=140&y=40
wait 800
menu-pick 132
wait 400
menu-pick 117
wait 400
close
EOF
  run_demo fake-link "$WORK/fake-link.log" "$FAKE"
  expect 'menu row 0 id=100 type=command enabled=0 checked=0 label=返回\(&B\)' \
    "$WORK/fake-link.log" fake-link
  expect 'menu row 2 id=-1 type=separator' "$WORK/fake-link.log" fake-link
  expect 'menu row 4 id=132 type=command enabled=1 .*查看网页源代码' "$WORK/fake-link.log" fake-link
  expect_count 'demo: menu row' "$WORK/fake-link.log" 5 fake-link
  expect 'context menu at 140,40 with 5 rows' "$WORK/fake-link.log" fake-link
  expect 'context menu with 5 items drawn at 140,40' "$WORK/fake-link.log" fake-link
  expect_mapped "$WORK/fake-link.log" fake-link
  expect 'fake: context-menu-select id=132 flags=0' "$WORK/fake-link.log" fake-link
  # The second pick has nothing to answer, and the session keeps that off the
  # channel: one command crossed, and the guard says the rest.
  expect_count 'fake: context-menu-select' "$WORK/fake-link.log" 1 fake-link
  expect 'no context menu to answer' "$WORK/fake-link.log" fake-link

  # 2. The editable model: two separators, so three sections, and the disabled
  #    rows a menu over an empty undo stack has.
  script fake-edit <<EOF
load http://gate.test/menu?menu=edit
wait 800
menu-dismiss
wait 400
close
EOF
  run_demo fake-edit "$WORK/fake-edit.log" "$FAKE"
  expect_count 'demo: menu row' "$WORK/fake-edit.log" 9 fake-edit
  expect_count 'type=separator' "$WORK/fake-edit.log" 2 fake-edit
  expect 'menu row 4 id=113 type=command enabled=0' "$WORK/fake-edit.log" fake-edit
  expect 'menu row 8 id=117 type=command enabled=1' "$WORK/fake-edit.log" fake-edit
  expect 'context menu with 9 items drawn' "$WORK/fake-edit.log" fake-edit
  expect 'fake: context-menu-dismiss' "$WORK/fake-edit.log" fake-edit

  # 3. The types the default model never shows. A check, a radio and a submenu all
  #    arrive as plain rows here, because the menu is rebuilt from a fresh model
  #    after every command and keeps no state of its own to mark.
  script fake-mixed <<EOF
load http://gate.test/menu?menu=mixed
wait 800
menu-pick 26501
wait 400
close
EOF
  run_demo fake-mixed "$WORK/fake-mixed.log" "$FAKE"
  expect 'menu row 0 id=200 type=check .*checked=1' "$WORK/fake-mixed.log" fake-mixed
  expect 'menu row 1 id=201 type=radio' "$WORK/fake-mixed.log" fake-mixed
  expect 'menu row 3 id=26501 type=command .*下划线_测试' "$WORK/fake-mixed.log" fake-mixed
  expect 'menu row 4 id=26502 type=submenu' "$WORK/fake-mixed.log" fake-mixed
  expect 'context menu with 5 items drawn' "$WORK/fake-mixed.log" fake-mixed
  expect 'fake: context-menu-select id=26501' "$WORK/fake-mixed.log" fake-mixed

  # 4. A model with no rows in it, and one that is only separators, are both not a
  #    menu. The session has to answer for them anyway, because an open request
  #    costs every later right click.
  script fake-empty <<EOF
load http://gate.test/menu?menu=empty
wait 800
load http://gate.test/menu?menu=seps
wait 800
menu-pick 100
wait 400
close
EOF
  run_demo fake-empty "$WORK/fake-empty.log" "$FAKE"
  expect_no 'type=command' "$WORK/fake-empty.log" fake-empty
  expect_count 'type=separator' "$WORK/fake-empty.log" 2 fake-empty
  expect_no 'items drawn' "$WORK/fake-empty.log" fake-empty
  expect_count 'fake: context-menu-dismiss' "$WORK/fake-empty.log" 2 fake-empty
  expect 'no context menu to answer' "$WORK/fake-empty.log" fake-empty

  # 5. The helper closing a request the host never answered: the menu comes down,
  #    and nothing goes back — the slot is empty over there already.
  script fake-auto <<EOF
load http://gate.test/menu?menu=auto
wait 1200
load http://gate.test/menu?menu=link
wait 800
menu-dismiss
wait 400
close
EOF
  run_demo fake-auto "$WORK/fake-auto.log" "$FAKE"
  expect 'context menu with 5 items drawn' "$WORK/fake-auto.log" fake-auto
  expect 'the helper closed the context menu' "$WORK/fake-auto.log" fake-auto
  expect 'context menu at 140,40 with 5 rows' "$WORK/fake-auto.log" fake-auto
  # One dismiss crossed the wire: the second menu's. The first was closed over by
  # the helper, and answering that would be an answer to nothing.
  expect_count 'fake: context-menu-dismiss' "$WORK/fake-auto.log" 1 fake-auto

  # 6. Two requests with no answer between them. Chromium is not supposed to do
  #    this, and the host still has to end up holding only the second one.
  script fake-double <<EOF
load http://gate.test/menu?menu=link&x=10&y=20
wait 800
load http://gate.test/menu?menu=selection&x=30&y=40
wait 800
menu-pick 113
wait 400
close
EOF
  run_demo fake-double "$WORK/fake-double.log" "$FAKE"
  expect 'a second context menu arrived before the first was answered' \
    "$WORK/fake-double.log" fake-double
  # The first menu is put down unanswered, so only the second is answered.
  expect_count 'fake: context-menu-select' "$WORK/fake-double.log" 1 fake-double
  expect 'fake: context-menu-select id=113' "$WORK/fake-double.log" fake-double
  expect_no 'fake: context-menu-dismiss' "$WORK/fake-double.log" fake-double
fi

# ---------------------------------------------------------------------------
# The real helper: what Chromium builds, and what a picked command does to it.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = real ]; then
  # 7. The contexts the fixture puts at known coordinates. Link and image get the
  #    helper's custom rows (and no Back/Forward/Print/View Source); the editable
  #    and selection keep Chromium's own models. Copy is picked from the link and
  #    image menus; 全选 is picked from the editable and the textarea reports that
  #    its selection grew — which is the whole point of Continue().
  script real-models <<EOF
load $PAGE
wait 3000
click 140 40 273
wait 1200
menu-pick 26502
wait 600
click 120 130 273
wait 1200
menu-pick 26504
wait 600
js window.selectEdit()
wait 600
click 100 250 273
wait 1200
menu-pick 117
wait 1200
js document.getElementById('edit').selectionStart + '-' + document.getElementById('edit').selectionEnd
wait 600
js window.selectText()
wait 600
click 400 45 273
wait 1200
menu-dismiss
wait 600
close
EOF
  run_demo real-models "$WORK/real-models.log" "$HELPER"
  expect 'loaded .*menu-page\.html \(HTTP 200\)' "$WORK/real-models.log" real-models
  expect 'context menu with 3 items at 140,40' "$WORK/real-models.log" real-models
  expect 'menu row 0 id=26501 type=command enabled=1' "$WORK/real-models.log" real-models
  expect 'menu row 1 id=26502 type=command enabled=1' "$WORK/real-models.log" real-models
  expect_mapped "$WORK/real-models.log" real-models
  expect 'context menu with 4 items at' "$WORK/real-models.log" real-models
  expect 'context menu with 4 items drawn' "$WORK/real-models.log" real-models
  expect 'context menu with 9 items drawn' "$WORK/real-models.log" real-models
  expect 'context menu with 1 items at 400,4[0-9]' "$WORK/real-models.log" real-models
  expect_count 'demo: copy-text https://example.invalid/target' "$WORK/real-models.log" 1 real-models
  expect_count 'demo: copy-text data:image/png' "$WORK/real-models.log" 1 real-models

  picked=$(js_value "$WORK/real-models.log" 2)
  if [ "$picked" = "0-18" ]; then
    ok "real-models: the picked command ran in Chromium (selection $picked)"
  else
    bad "real-models: selection after 全选 was '${picked:-none}', wanted 0-18"
  fi

  # 8. A navigation with the menu still open. Chromium leaves an owned request
  #    alone, so the session has to close it — otherwise the popover sits over the
  #    new page and no later right click reaches the host again.
  script real-nav <<EOF
load $PAGE
wait 3000
click 140 40 273
wait 1200
load $OTHER
wait 2500
click 140 40 273
wait 1200
menu-dismiss
wait 600
close
EOF
  run_demo real-nav "$WORK/real-nav.log" "$HELPER"
  expect 'context menu dismissed by a navigation' "$WORK/real-nav.log" real-nav
  expect_count 'context menu with 3 items drawn' "$WORK/real-nav.log" 1 real-nav
  expect_count 'context menu with 5 items drawn' "$WORK/real-nav.log" 1 real-nav
  # The first is the link (custom rows); the second is the other page's blank
  # background, whose 返回 is enabled by the navigation that dismissed the menu.
  expect 'menu row 0 id=26501 type=command enabled=1' "$WORK/real-nav.log" real-nav
  expect 'menu row 0 id=100 type=command enabled=1' "$WORK/real-nav.log" real-nav
fi

echo
if [ $fails -eq 0 ] && [ $skips -gt 0 ]; then
  printf 'menu gate: all checks passed, %d skipped\n' "$skips"
elif [ $fails -eq 0 ]; then
  printf 'menu gate: all checks passed\n'
else
  printf 'menu gate: %d check(s) failed\n' "$fails"
  exit 1
fi
