#!/usr/bin/env bash
#
# test/dnd-gate.sh — drives the in-page drag loop and asserts on it.
#
# All six runs need the real browser: what is under test is the choreography
# between a page's HTML5 drag and wl_data_device, and only Chromium does the
# page half of it. The fake helper has no window to drag inside.
#
#   1. a drag that ends over a drop target — every event the page should see, in
#      order, at the coordinates the script asked for, and the payload back on
#      the far side;
#   2. a drag that ends over an element which will not take it — dragend, no drop;
#   3. two drags in one session — the second starts from what the first left
#      behind rather than from the first's objects;
#   4. a drag held open — the drag image appears in the paint stack while the
#      button is down and is gone after the release, and the page takes a click
#      afterwards, which is how the seat is shown to have been handed back;
#   5. a drag that leaves the window — the compositor reads the payload itself,
#      calls the page's drag off and hands the whole thing to the host, and the
#      page sees a leave and an end with no drop in between;
#   6. a drag the host carries in — the page is the destination instead of the
#      source, and the four endings a destination can give are told apart: taken
#      over a drop target, given back over an element that handles nothing, given
#      back when the host calls it off, and taken again after all of that.
#
# It opens a real demo window per run, so it is a documented manual gate rather
# than a meson test — the same convention as test/*.session,
# test/download-gate.sh and test/find-gate.sh. Nothing here moves a pointer on
# the host desktop; but a scripted drag shares the one seat with the real
# pointer, so if a hand is on the mouse over the window its motions join the
# log. That is why the assertions are about what arrives rather than how many
# times, except where a count is the point.
#
# The compositor's own trace is asserted as well, and it needs G_MESSAGES_DEBUG
# plus a stderr that is not a plain file: GLib's default writer drops DEBUG
# messages when the stream is a regular file, so every run here pipes its stderr
# through tee.
#
#   ./test/dnd-gate.sh
#   ./test/dnd-gate.sh loop      # only one of the six
#
# The protocol-error half is already covered by meson test: the
# seat-selftest-dnd-requests case sends wl_data_device_manager version 3
# requests at the compositor and asserts that both connections stay alive,
# seat-selftest-drag-out drives the handover of run 5 from a fake client that
# offers the same three types, and seat-selftest-drag-in drives run 6 from a fake
# client that is an active destination — a type accepted, the payload read twice
# and the offer finished — with no browser and no window in the loop.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
HELPER="$ROOT/helper/build/Release/cefnest-helper"
PAGE="file://$ROOT/test/dnd-page.html"
WHICH=${1:-all}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fails=0

ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qaE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -an 'demo: script\|nested: ' "$2" | tail -6 | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qaE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -an "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

expect_occ() { # expect_occ <pattern> <logfile> <count> <tag>
  local n
  n=$(grep -oaE "$1" "$2" | wc -l | tr -d ' ')
  if [ "$n" = "$3" ]; then ok "$4: $n occurrence(s) of /$1/"
  else bad "$4: $n occurrence(s) of /$1/, wanted $3"; fi
}

# The page reads its own log back through a js step, which prints it as one
# value on one line. Pulling that line out keeps the assertions about order
# honest: they run against the page's array, not against the trace.
page_logs() { # page_logs <logfile> — every dndLog the page reported, one per line
  sed -n 's/^demo: script [0-9]* ok value="\(.*\)"$/\1/p' "$1"
}

page_log() { # page_log <logfile> — the last one
  page_logs "$1" | tail -1
}

run_demo() { # run_demo <tag> <logfile> <helper> <extra env...>
  local tag=$1 log=$2 helper=$3
  shift 3

  if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
  if [ ! -x "$helper" ]; then echo "no helper at $helper — run cmake --build helper/build"; exit 1; fi
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

# A protocol error is fatal for whichever end noticed it, so none of these runs
# may contain one. libwayland prints every protocol error as "<iface>@<id>:
# error <code>: <message>", which is what this looks for.
no_protocol_error() { # no_protocol_error <logfile> <tag>
  expect_no '@[0-9]+: error [0-9]+' "$1" "$2"
}

# ---------------------------------------------------------------------------
# 1. The loop: a drag from the box onto the page, with the payload crossing.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = loop ]; then
  script loop <<EOF
load $PAGE
wait 4000
js window.dndLog.join('|')
drag 150 100 150 500
wait 3000
js window.dndLog.join('|')
wait 1500
quit
EOF
  run_demo loop "$WORK/loop.log" "$HELPER"

  # Before the drag there is nothing to assert about, and saying so is the
  # baseline the rest of the run is measured against.
  if [ "$(page_logs "$WORK/loop.log" | head -1)" = "" ]; then
    ok "loop: the page started with an empty log"
  else
    bad "loop: the page logged something before the drag"
  fi

  log=$(page_log "$WORK/loop.log")
  if [ -z "$log" ]; then
    bad "loop: the page never reported its log"
  else
    for want in 'dragstart\[text/plain\]@source 150,100' \
                'dragenter\[[^]]*\]@target' \
                'dragover\[[^]]*\]@target' \
                'drop\(dnd-payload\)\[[^]]*\]@target 150,49[0-9]' \
                'dragend\[text/plain\]@source'; do
      if printf '%s' "$log" | grep -qaE "$want"; then
        ok "loop: the page saw /$want/"
      else
        bad "loop: the page never saw /$want/ — log was $log"
      fi
    done
    # The order is the protocol's: entered, hovered, dropped, ended — with
    # dragstart first and dragend last. What happens in between is allowed to
    # vary, because a pointer that is really on the mouse can move inside the
    # window while the script is moving it too, and each of those motions is a
    # destination change.
    order=$(printf '%s' "$log" | tr '|' '\n' |
            sed 's/\[[^]]*\]//; s/@.*//; s/(.*)//' | tr '\n' ' ' |
            sed 's/ *$//')
    if printf '%s' "$order" | grep -qaE '^dragstart ((dragenter|dragleave|dragover) )+drop dragend$'; then
      ok "loop: the events arrived in the order the protocol asks for"
    else
      bad "loop: unexpected event order: $order"
    fi
  fi

  # The payload is the whole point of the run: the page read back the string the
  # drag started with, which it could not do unless the offer named it and the
  # data reached it.
  expect 'drop\(dnd-payload\)' "$WORK/loop.log" loop

  expect 'nested: a drag started over surface id [0-9]+ carrying 3 type' "$WORK/loop.log" loop
  expect 'nested: dragged onto surface id [0-9]+ offering' "$WORK/loop.log" loop
  # What the destination settled on, measured: Chromium offers copy and takes
  # copy, and it accepts its own chromium/x-web-custom-data type.
  expect 'nested: drag settled on action 0x1 of source 0x1 and destination 0x1' "$WORK/loop.log" loop
  expect 'nested: dropped chromium/x-web-custom-data as action 0x1' "$WORK/loop.log" loop
  expect 'nested: a drag concluded' "$WORK/loop.log" loop
  no_protocol_error "$WORK/loop.log" loop
fi

# ---------------------------------------------------------------------------
# 2. A destination that will not take the payload.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = refused ]; then
  # (150,90) never leaves the source box, and the box handles no drop event at
  # all, so the page has nowhere to put the payload.
  script refused <<EOF
load $PAGE
wait 4000
drag 150 100 150 90
wait 3000
js window.dndLog.join('|')
wait 1500
quit
EOF
  run_demo refused "$WORK/refused.log" "$HELPER"

  log=$(page_log "$WORK/refused.log")
  expect 'dragstart' "$WORK/refused.log" refused
  expect 'dragend' "$WORK/refused.log" refused
  if printf '%s' "$log" | grep -qaE 'drop\('; then
    bad "refused: the page reported a drop it should not have had — log was $log"
  else
    ok "refused: no drop reached the page"
  fi
  # The distinction that survives a second pointer in the room is the payload:
  # nothing was read, because nothing was asked for.
  if printf '%s' "$log" | grep -qaE 'dnd-payload'; then
    bad "refused: the payload crossed anyway — log was $log"
  else
    ok "refused: the payload stayed with the source"
  fi

  expect 'nested: a drag started over surface' "$WORK/refused.log" refused
  # Measured: the destination accepts an internal mime and then says nothing
  # about actions, so the compositor sends the drop and lets the page refuse it
  # in the way a page refuses things. The drag still ends.
  expect 'nested: button 0x110 released: the drag is let go' "$WORK/refused.log" refused
  no_protocol_error "$WORK/refused.log" refused
fi

# ---------------------------------------------------------------------------
# 3. A second drag after the first.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = twice ]; then
  script twice <<EOF
load $PAGE
wait 4000
drag 150 100 150 500
wait 2000
drag 150 100 150 500
wait 3000
js window.dndLog.join('|')
wait 1500
quit
EOF
  run_demo twice "$WORK/twice.log" "$HELPER"

  # Each drag gets its own source, its own offer and its own conclusion. Counting
  # occurrences inside the page's own line is the point here: a drag that reused
  # the first one's objects would show as one drop, or as a dead connection.
  expect_occ 'drop\(dnd-payload\)' "$WORK/twice.log" 2 twice
  expect_occ 'nested: a drag started over surface' "$WORK/twice.log" 2 twice
  expect_occ 'nested: a drag concluded' "$WORK/twice.log" 2 twice
  no_protocol_error "$WORK/twice.log" twice
fi

# ---------------------------------------------------------------------------
# 4. The drag image, and the seat afterwards.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = held ]; then
  script held <<EOF
load $PAGE
wait 4000
surfaces
drag-hold 150 100 150 500
wait 600
surfaces
release
wait 1500
surfaces
click 400 400
wait 1500
js window.dndLog.join('|') + ' clicks=' + window.clicks
wait 1500
quit
EOF
  run_demo held "$WORK/held.log" "$HELPER"

  # One surface before the drag, the page's image added while the button is down,
  # and back to one after it: the image is retired rather than left to follow the
  # pointer as a ghost.
  expect_occ 'demo: 1 surface\(s\) paint' "$WORK/held.log" 2 held
  expect_occ 'demo: 2 surface\(s\) paint' "$WORK/held.log" 1 held
  expect 'demo: surface 2 at [0-9-]+,[0-9-]+ size [0-9]+x[0-9]+' "$WORK/held.log" held

  expect 'drop\(dnd-payload\)' "$WORK/held.log" held
  # The drag held the pointer; the click says the seat gave it back.
  expect 'click@target 400,400 clicks=1' "$WORK/held.log" held
  no_protocol_error "$WORK/held.log" held
fi

# ---------------------------------------------------------------------------
# 5. A drag that leaves the window.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = out ]; then
  # 900 is below the widget, so the pointer runs out of the page on the way down
  # and the compositor is left holding a drag whose destination is not one of its
  # clients. CEFNEST_DRAG_OUT=take stops the demo at the read: carrying the
  # payload on into a real GdkDrag needs a button press GDK can cite, and no
  # script can give it one, so that leg is measured by hand.
  script out <<EOF
load $PAGE
wait 4000
js window.dndLog=[]; 'reset'
drag 150 100 150 900
wait 1500
js window.dndLog.join('|')
wait 1000
quit
EOF
  run_demo out "$WORK/out.log" "$HELPER" CEFNEST_DRAG_OUT=take

  # The three lines the handover is made of, in the shape measured off this page:
  # one type picked out of three, the payload the page set, and the host told.
  expect 'nested: the pointer left the page mid-drag: taking text/plain;charset=utf-8 of 3 types' "$WORK/out.log" out
  expect 'nested: the drag left the page as text/plain;charset=utf-8 \(11 bytes, 3 types\)' "$WORK/out.log" out
  expect 'demo: drag out: text/plain;charset=utf-8, 11 bytes, types: text/plain;charset=utf-8\|text/plain\|chromium/x-web-custom-data' "$WORK/out.log" out
  expect 'demo: drag out: taken, not carried' "$WORK/out.log" out

  # The page's side, measured: it is told the pointer left and then ends, and the
  # payload never comes back into it — it went to the host instead.
  log=$(page_log "$WORK/out.log")
  if printf '%s' "$log" | grep -qaE 'drop\('; then
    bad "out: the page took a drop it should not have had — log was $log"
  else
    ok "out: no drop reached the page"
  fi
  for want in 'dragleave\[[^]]*\]@target' 'dragend\[text/plain\]@source'; do
    if printf '%s' "$log" | grep -qaE "$want"; then
      ok "out: the page saw /$want/"
    else
      bad "out: the page never saw /$want/ — log was $log"
    fi
  done
  if printf '%s' "$log" | grep -qaE 'dnd-payload'; then
    bad "out: the payload came back into the page — log was $log"
  else
    ok "out: the payload stayed out of the page"
  fi

  expect_no 'nested: dropped' "$WORK/out.log" out
  no_protocol_error "$WORK/out.log" out
fi

# ---------------------------------------------------------------------------
# 6. A drag the host carries in.
# ---------------------------------------------------------------------------

if [ "$WHICH" = all ] || [ "$WHICH" = in ]; then
  # No real GdkDrop is involved: the outer leg would need a press in another
  # window, which no script can give GDK. The drop-in steps call the same three
  # entry points a drop target does, with the payload this process holds in
  # memory, so the page is the destination for once and the compositor is the
  # source. Four endings are measured in one run because the point of the
  # direction is the verdict the host has to pass back: taken over the target,
  # given back over the box that handles nothing, given back when the host calls
  # it off, and taken again afterwards.
  script in <<EOF
load $PAGE
wait 4000
js window.dndLog=[]; 'clear'
drop-in 400 300 400 360
wait 1500
js window.dndLog.join('|')
wait 500
js window.dndLog=[]; 'clear2'
drop-in 150 60 150 140
wait 1500
js window.dndLog.join('|')
wait 500
js window.dndLog=[]; 'clear3'
drop-in-hold 400 300 400 360
wait 800
drop-in-cancel
wait 1200
js window.dndLog.join('|')
wait 500
js window.dndLog=[]; 'clear4'
drop-in 400 300 400 360
wait 1500
js window.dndLog.join('|')
wait 1000
quit
EOF
  run_demo in "$WORK/in.log" "$HELPER"

  # The arrival, four times over: the bytes and the two names, and a surface
  # entered with an offer the page never asked for.
  expect_occ 'nested: the host brought a drag in: 9 bytes of 2 types' "$WORK/in.log" 4 in
  expect_occ 'nested: dragged onto surface id [0-9]+ offering the host.s payload' "$WORK/in.log" 4 in
  expect_occ 'demo: drop-in text/plain;charset=utf-8 at [0-9]+,[0-9]+: offered' "$WORK/in.log" 4 in
  # Measured: the page asks for the payload the moment it is entered, twice,
  # whether or not it then takes the drop. That is why the verdict cannot be
  # built on the read.
  expect_occ 'nested: handing the page 9 bytes of the host.s drag' "$WORK/in.log" 8 in

  # The two endings the compositor saw, and the two the host was told.
  expect_occ 'nested: dropped text/plain as action 0x1' "$WORK/in.log" 2 in
  expect_occ 'nested: dropped text/plain as action 0x0' "$WORK/in.log" 1 in
  expect_occ 'nested: a drag concluded' "$WORK/in.log" 2 in
  expect_occ 'nested: a drag ended without the destination finishing' "$WORK/in.log" 1 in
  expect_occ 'demo: drop-in verdict: taken, text/plain' "$WORK/in.log" 2 in
  expect_occ 'demo: drop-in verdict: given back' "$WORK/in.log" 2 in

  # The page's own account, one line per drag. The source box has no drop
  # listeners and is not inside #target, so the second line is empty: a page
  # that is entered but handles nothing says nothing, which is exactly the case
  # the verdict has to catch.
  taken=$(page_logs "$WORK/in.log" | grep -v '^clear' | sed -n 1p)
  boxed=$(page_logs "$WORK/in.log" | grep -v '^clear' | sed -n 2p)
  back=$(page_logs "$WORK/in.log" | grep -v '^clear' | sed -n 3p)
  again=$(page_logs "$WORK/in.log" | grep -v '^clear' | sed -n 4p)

  for want in 'dragenter\[text/plain\]@target 400,300' \
              'dragover\[text/plain\]@target 400,3[0-6][0-9]' \
              'drop\(from-host\)\[text/plain\]@target'; do
    if printf '%s' "$taken" | grep -qaE "$want"; then
      ok "in: the page saw /$want/"
    else
      bad "in: the page never saw /$want/ — log was $taken"
    fi
  done

  if printf '%s' "$boxed" | grep -qaE 'drop\(|dragenter'; then
    bad "in: the box that handles nothing reported /$boxed/"
  else
    ok "in: the element with no drop handler said nothing, as it does"
  fi

  if printf '%s' "$back" | grep -qaE 'dragenter\[text/plain\]@target' &&
     printf '%s' "$back" | grep -qaE 'dragleave\[text/plain\]@target'; then
    ok "in: the called-off drag reached the page as an enter and a leave"
  else
    bad "in: the called-off drag did not reach the page — log was $back"
  fi
  if printf '%s' "$back" | grep -qaE 'drop\('; then
    bad "in: a cancelled drag still took a drop — log was $back"
  else
    ok "in: no drop came from a called-off drag"
  fi

  # Last, and the one that says the three endings left nothing behind: a page the
  # host can drop on once more after all of them.
  if printf '%s' "$again" | grep -qaE 'drop\(from-host\)\[text/plain\]@target'; then
    ok "in: a fourth drag arrived after the other three ended"
  else
    bad "in: the drag after the endings never dropped — log was $again"
  fi

  no_protocol_error "$WORK/in.log" in
fi

echo
if [ $fails -eq 0 ]; then
  printf 'dnd gate: all checks passed\n'
else
  printf 'dnd gate: %d check(s) failed\n' "$fails"
  exit 1
fi
