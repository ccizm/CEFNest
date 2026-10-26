#!/usr/bin/env bash
#
# test/plugin-gate.sh — the P1 gate: the native frontend end to end.
#
# Four runs against the real CEF helper and the demo with examples/plugin-demo
# assembled into a plugin directory (the id is the directory name, so the gate
# picks "demo"):
#
#   1. baseline      no --plugin-dir: no plugin menu rows anywhere, and the
#                    helper never hears a menu verb
#   2. native        the plugin loads, its row is in the menu, picking it
#                    reaches the plugin's own callback, and disable() takes
#                    both the row and the .so back out
#   3. escape        the extension scheme's path check: a literal ../ form is
#                    normalised into the root and answers 404, an encoded
#                    ..%2F form reaches the handler and is refused with 403,
#                    the escape is named on the channel, and no byte of the
#                    secret outside the root is ever served
#   4. cs            content scripts: the matched page runs the plugin's
#                    document_start script before its own inline script, the
#                    storage round trip works, the unmatched page sees none
#                    of it — and the inject cost is logged for §六.3
#   5. noperm        a plugin that declares nothing calling chrome.storage is
#                    refused by name, the refusal lands on the manager, and
#                    the other plugin's storage keeps working
#   6. datadir       the data-directory contract: storage without a data
#                    directory writes nothing anywhere; with one, storage.json
#                    lands under cefnest-plugins/<id>/ and clear_data()
#                    takes the whole tree
#   8. payload       200 KiB round-trips, 1.5 MiB is refused by name before
#                    the channel sees it, and a small call right after still
#                    works — the channel survives an oversized plugin
#   9. refusals      three broken manifests through the real directory get
#                    the same tokens the model tests pin down
#   7. unload        disable, restart, re-enable, restart: the .so unloads on
#                    both the disable and the helper going away, reloads on
#                    the next enable and on ::ready, the menu row comes back,
#                    and G_DEBUG=fatal-criticals holds for the whole run
#
#   ./test/plugin-gate.sh
#   ./test/ci-session.sh ./test/plugin-gate.sh
#   KEEP_LOGS=1 ./test/plugin-gate.sh

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
PLUGIN_SO="$ROOT/builddir/examples/plugin-demo/libplugin-demo.so"
PLUGIN_MANIFEST="$ROOT/examples/plugin-demo/cefnest-plugin.json"
PAGE="file://$ROOT/test/menu-page.html"
. "$ROOT/test/helper-path.sh"

if [ "${CEFNEST_HEADLESS:-0}" = 1 ] && [ -z "${PLUGIN_GATE_XVFB:-}" ]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    export PLUGIN_GATE_XVFB=1
    exec xvfb-run -a env -u WAYLAND_DISPLAY GDK_BACKEND=x11 "$0" "$@"
  fi
  printf '  skip plugin: no xvfb-run, and headless mutter does not return from gtk_popover_popup()\n'
  printf 'plugin gate: skipped, it could not assert anything\n'
  exit 77
fi

[ -x "$DEMO" ] || { echo "no demo at $DEMO — run ninja -C builddir"; exit 1; }
[ -x "$HELPER" ] || { echo "no helper at $HELPER"; exit 1; }
[ -f "$PLUGIN_SO" ] || { echo "no plugin module at $PLUGIN_SO — run ninja -C builddir"; exit 1; }

WORK=$(mktemp -d)
KEEP=${KEEP_LOGS:-0}
cleanup() {
  if [ "$KEEP" = 1 ]; then
    echo "  note: transcripts kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

# The plugin directory the gate installs from: manifest plus the module the
# build produced. Nothing in the tree has to exist under the id the gate wants.
mkdir -p "$WORK/plugins/demo"
cp "$PLUGIN_MANIFEST" "$WORK/plugins/demo/cefnest-plugin.json"
cp "$PLUGIN_SO" "$WORK/plugins/demo/libplugin-demo.so"
cp "$ROOT/examples/plugin-demo/cs.js" "$WORK/plugins/demo/cs.js"
cp "$ROOT/examples/plugin-demo/popup.html" "$WORK/plugins/demo/popup.html"

# The no-permissions plugin: inert everywhere except its own page.
mkdir -p "$WORK/plugins/noperm"
cp "$ROOT/examples/plugin-noperm/cefnest-plugin.json" "$WORK/plugins/noperm/"
cp "$ROOT/examples/plugin-noperm/cs-noperm.js" "$WORK/plugins/noperm/"

# The refusals, gate 9: three manifests that are broken in the three ways the
# model tests pin down, installed through a real directory.
for broken in badver badtype bgserv; do
  mkdir -p "$WORK/broken/$broken"
done
printf '{"manifest_version": 3, "name": "x"}\n' \
  > "$WORK/broken/badver/cefnest-plugin.json"
printf '{"manifest_version": 3, "name": "x", "version": "1", "cefnest": {"type": "worker"}}\n' \
  > "$WORK/broken/badtype/cefnest-plugin.json"
printf '{"manifest_version": 3, "name": "x", "version": "1", "background": {"service_worker": "sw.js"}, "cefnest": {"type": "web"}}\n' \
  > "$WORK/broken/bgserv/cefnest-plugin.json"

# A file deliberately outside every plugin root, for the escape run to want.
printf 'SECRET-OUTSIDE-ROOT\n' > "$WORK/plugins/escape.txt"

# The helper, Chromium's cache and the demo's nested-compositor socket all land
# in XDG_RUNTIME_DIR; give them one nobody else reads.
mkdir -p "$WORK/xdg"
chmod 700 "$WORK/xdg"
export XDG_RUNTIME_DIR="$WORK/xdg"

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    tail -20 "$2" | sed 's/^/         /'
  fi
}

expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

run_demo() { # run_demo <tag> <script> [demo args...]
  local tag=$1 script=$2
  shift 2
  echo "== $tag"
  CHROMIUM_SESSION_SCRIPT=$WORK/$script CHROMIUM_HELPER=$HELPER \
    timeout --kill-after=10s 120 "$DEMO" "$@" </dev/null >"$WORK/$tag.log" 2>&1
  local rc=$?
  if [ "$rc" = 0 ]; then ok "$tag: demo exited 0"
  else bad "$tag: demo exited $rc"; tail -20 "$WORK/$tag.log" | sed 's/^/         /'; fi
}

# The right click opens the session's own menu popover; menu-pick answers the
# row by its id. Coordinates are the page view's own.
write_script() { # write_script <file> <body>
  printf '%s\n' "$2" > "$WORK/$1"
}

# ---------------------------------------------------------------- gate 1 ----

write_script baseline.script "load $PAGE
wait 2500
click 600 400 273
wait 1500
menu-dismiss
wait 500
quit"

run_demo baseline baseline.script
log=$WORK/baseline.log
expect 'demo: context menu at 600,400 with [0-9]+ rows' "$log" baseline
expect_no 'id=266[0-9][0-9]' "$log" baseline
expect_no 'plugin: native' "$log" baseline
expect_no 'helper: plugin menu row' "$log" baseline
expect_no 'demo: plugin' "$log" baseline

# ---------------------------------------------------------------- gate 2 ----

write_script native.script "load $PAGE
wait 2500
click 600 400 273
wait 1500
menu-pick 26600
wait 1000
plugin-disable demo
wait 500
click 600 400 273
wait 1500
menu-dismiss
wait 500
quit"

run_demo native native.script --plugin-dir="$WORK/plugins"
log=$WORK/native.log
# The scan prints the row before the demo's own enable step runs, so the
# state on the line is scanned; the enable is the two lines after it.
expect 'demo: plugin demo name=Plugin Demo kind=native state=scanned' "$log" native
expect 'demo: plugin demo enabled' "$log" native
expect 'plugin: native "demo" loading' "$log" native
expect 'helper: plugin menu row slot=0 label=' "$log" native
expect 'demo: menu row [0-9]+ id=26600 type=command enabled=1 checked=0 label=插件示例：命中记录' \
       "$log" native
expect 'demo: menu pick id=26600' "$log" native
# The callback runs in the demo's own process: its print lands in this log.
expect 'plugin-demo: menu command slot=0 params=' "$log" native
# The plugin also subscribed to the session's ::load-end through the registry.
expect 'plugin-demo: page loaded' "$log" native
# disable() unloads the .so with everything it registered.
expect 'plugin: native "demo" unloading \(1 menu rows, 1 connections\)' "$log" native
expect 'helper: plugin menu row removed slot=0' "$log" native
# The next menu has no plugin row in it.
last_row=$(grep -E '^demo: menu row [0-9]+ id=' "$log" | tail -1)
case "$last_row" in
  *'id=266'*) bad "native: the disabled plugin's row is still on the menu: $last_row" ;;
  '') bad "native: the second menu printed no rows at all" ;;
  *) ok "native: the menu after disable ends at $last_row" ;;
esac

# ---------------------------------------------------------------- gate 3 ----

# The same run serves both probes: the literal form first (URL normalisation
# clamps the dots at the root, so the handler sees a name inside the root and
# answers 404), then the encoded form (which arrives at the handler still
# holding the dots and is refused with 403 — the one case the prefix check
# exists for).
write_script escape.script "load cefnest-extension://demo/../../../../etc/passwd
wait 2000
load cefnest-extension://demo/..%2F..%2Fescape.txt
wait 2000
load cefnest-extension://nosuchplugin/anything.html
wait 2000
quit"

run_demo escape escape.script --plugin-dir="$WORK/plugins"
log=$WORK/escape.log
expect 'helper: plugins load id=demo root=' "$log" escape
# The literal dots never reach the handler as dots; the miss is a plain 404.
expect 'helper: extension missing status=404 url=[^ ]*demo/etc/passwd' "$log" escape
# The encoded form is the escape itself, refused and named.
expect 'helper: extension-path-escape status=403 url=[^ ]*demo/\.\.%2F\.\.%2Fescape\.txt' "$log" escape
# A plugin the channel never told the helper about gets the same 404 as a
# missing file, not a 403: unknown roots are absence, not an attack.
expect 'helper: extension unknown-plugin status=404 url=[^ ]*nosuchplugin/' "$log" escape
# And the point of all of it: not one byte of the file outside the root.
expect_no 'SECRET-OUTSIDE-ROOT' "$log" escape
expect_no 'root:x:0:0' "$log" escape
expect_no 'served status=200 url=[^ ]*(escape\.txt|passwd)' "$log" escape

# ---------------------------------------------------- the anchored root ----

# The install root rides on the data directory, not on the component's name:
# a profile at --data-dir gets its cefnst-plugins/ scanned with no --plugin-dir
# at all, and a profile without that directory scans as empty rather than
# failing. This run installs the plugin the way a host would — under the data
# directory — and starts the demo with nothing but --data-dir.
mkdir -p "$WORK/data/cefnest-plugins/demo"
cp "$PLUGIN_MANIFEST" "$WORK/data/cefnest-plugins/demo/cefnest-plugin.json"
cp "$PLUGIN_SO" "$WORK/data/cefnest-plugins/demo/libplugin-demo.so"

write_script dataroot.script "load $PAGE
wait 2500
click 600 400 273
wait 1500
menu-pick 26600
wait 1000
quit"

run_demo dataroot dataroot.script --data-dir="$WORK/data"
log=$WORK/dataroot.log
expect 'demo: plugin demo name=Plugin Demo kind=native state=scanned' "$log" dataroot
expect 'demo: plugin demo enabled' "$log" dataroot
expect 'helper: plugins load id=demo root='"$WORK"'/data/cefnest-plugins/demo' "$log" dataroot
expect 'demo: menu row [0-9]+ id=26600' "$log" dataroot
expect 'demo: menu pick id=26600' "$log" dataroot
expect 'plugin-demo: menu command slot=0' "$log" dataroot
# And a scan failure would have printed; there was nothing to fail on.
expect_no 'demo: plugin scan:' "$log" dataroot

# ---------------------------------------------------------------- gate 4 ----

# Content scripts. The hit page's title tells the whole story: the plugin's
# document_start script writes CS-HIT, the storage round trip turns it into
# CS-OK:read-back, and the page's own inline script appends page-script after
# — script first, page second, which is the document_start order the P0 run
# measured. The helper's inject line carries the §六.3 number.
write_script cs.script "load file://$ROOT/test/plugin-page-hit.html
wait 2500
js document.title
wait 500
load file://$ROOT/test/menu-page.html
wait 2000
js document.title
wait 500
quit"

run_demo cs cs.script --plugin-dir="$WORK/plugins"
log=$WORK/cs.log
expect 'helper: cs inject browser=1 .* scripts=[0-9]+ us=[0-9]+' "$log" cs
expect 'demo: script [0-9]+ ok value="CS-OK:read-back:ran-first"' "$log" cs
# The unmatched page keeps its own title — no plugin script ever touched it.
expect 'demo: script [0-9]+ ok value="MENU-PAGE"' "$log" cs
# The unmatched page must not have run any plugin script: no CS anywhere near
# the second load.
expect_no 'CS-HIT' "$log" cs
inject_us=$(grep -o 'cs inject browser=1 [^ ]* scripts=[0-9]* us=[0-9]*' "$log" |
            head -1 | grep -o 'us=[0-9]*' | cut -d= -f2)
if [ -n "$inject_us" ]; then
  ok "cs: the real-size inject cost is recorded (us=$inject_us)"
  printf '       §六.3 re-measure: %s µs for the full matching + injection block\n' "$inject_us"
else
  bad "cs: no inject timing line for §六.3"
fi

# ---------------------------------------------------------------- gate 5 ----

# A plugin that declares nothing calling chrome.storage: the shim answers the
# page, the permission table answers the shim, and the refusal is on the
# manager where the row is.
write_script noperm.script "load file://$ROOT/test/plugin-noperm-page.html
wait 2500
js document.title
wait 500
quit"

run_demo noperm noperm.script --plugin-dir="$WORK/plugins"
log=$WORK/noperm.log
expect 'demo: script [0-9]+ ok value="NOPERM-DENIED"' "$log" noperm
expect 'demo: plugin error noperm: permission-denied:storage' "$log" noperm

# ---------------------------------------------------------------- gate 6 ----

# The data-directory contract, both halves. First: with a data directory, the
# storage the content script wrote lands under cefnst-plugins/<id>/ and
# clear_data() takes the whole tree. Second: without a data directory, the
# same write leaves nothing anywhere.
write_script datadir.script "load file://$ROOT/test/plugin-page-hit.html
wait 2000
restart
wait 3000
clear-data
wait 2000
quit"

run_demo datadir datadir.script --plugin-dir="$WORK/plugins" --data-dir="$WORK/data"
log=$WORK/datadir.log
expect 'demo: data cleared' "$log" datadir
[ -e "$WORK/data/cefnest-plugins" ] &&
  { bad "datadir: clear_data left cefnest-plugins behind"; } ||
  ok "datadir: clear_data took cefnest-plugins with the tree"

# Storage persisted at all: a run that only writes, then exits, leaves the
# file — and its content is what the shim wrote, nothing more.
write_script persist.script "load file://$ROOT/test/plugin-page-hit.html
wait 2000
quit"

run_demo persist persist.script --plugin-dir="$WORK/plugins" --data-dir="$WORK/data2"
log=$WORK/persist.log
if [ -f "$WORK/data2/cefnest-plugins/demo/storage.json" ] &&
   grep -q '"cs"' "$WORK/data2/cefnest-plugins/demo/storage.json"; then
  ok "persist: storage.json under cefnest-plugins/demo with the cs key"
else
  bad "persist: no storage.json where the contract says it lands"
fi

# And the no-data-directory half: the same content script, no data directory,
# and nothing appears under the run's HOME.
mkdir -p "$WORK/home/.cache" "$WORK/home/.local/share"
write_script nodisk.script "load file://$ROOT/test/plugin-page-hit.html
wait 2000
quit"

echo "== nodisk"
find "$WORK/home" -type f | grep -v "/\.cache/" | sort > "$WORK/home-before.txt"
CHROMIUM_SESSION_SCRIPT=$WORK/nodisk.script CHROMIUM_HELPER=$HELPER \
  HOME="$WORK/home" XDG_CACHE_HOME="$WORK/home/.cache" \
  XDG_DATA_HOME="$WORK/home/.local/share" XDG_CONFIG_HOME="$WORK/home/.config" \
  timeout --kill-after=10s 120 "$DEMO" --plugin-dir="$WORK/plugins" </dev/null >"$WORK/nodisk.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then ok "nodisk: demo exited 0"
else bad "nodisk: demo exited $rc"; fi
# What the run's own stack writes under HOME (the graphics caches, Chromium's
# NSS database, the desktop's gvfs metadata) is not the plugin's doing, so
# the assertion targets the plugin contract itself: without a data directory
# nothing named cefnest-plugins — no storage.json, no plugin tree — may exist
# anywhere under HOME. That is the claim the data-directory contract makes.
if find "$WORK/home" -name "cefnest-plugins" -o -name "storage.json" |
     grep -q .; then
  bad "nodisk: plugin storage landed on disk without a data directory"
  find "$WORK/home" -name "cefnest-plugins" -o -name "storage.json" |
    head -5 | sed 's/^/         /'
else
  ok "nodisk: no plugin storage anywhere under HOME"
fi

# ---------------------------------------------------------------- gate 8 ----

# The payload ceiling: 200 KiB round-trips, 1.5 MiB is refused on the page
# side before the channel ever sees it, and the small call after proves the
# channel is alive.
write_script payload.script "load file://$ROOT/test/plugin-page-payload.html
wait 3000
js document.title
wait 500
quit"

run_demo payload payload.script --plugin-dir="$WORK/plugins"
log=$WORK/payload.log
expect 'demo: script [0-9]+ ok value="PAYLOAD-TOOBIG-then-OK"' "$log" payload
expect_no 'PAYLOAD-BIG-ACCEPTED' "$log" payload
expect_no 'PAYLOAD-DEAD' "$log" payload

# ---------------------------------------------------------------- gate 9 ----

# tabs 扩面：两个浏览器，一个静音一个有声，一个 tab 一个独立窗口；query 的
# 字段各自正确，tabs.update 的 muted 把指定 tab 切回去。页面把 query 的
# 结果写进 title，js 步骤读回 —— chrome.tabs.query 的 Promise 没人 await，
# 只有页面自己的 .then 能把答案放到一个后续步骤能读的地方。
write_script tabs.script "load file://$ROOT/test/plugin-page-tabs.html
wait 2500
mute
new-window file://$ROOT/test/plugin-page-tabs.html
wait 2500
zoom-set 1.5
js chrome.tabs.query({}).then(function (tabs) { document.title = 'TABS-OK:' + tabs.map(function (t) { return t.id + ':' + (t.windowed ? 'w' : 't') + ':' + (t.muted ? 'm' : 's') + ':' + t.zoom; }).join('|'); })
wait 800
js document.title
wait 500
js chrome.tabs.update(1, {muted: false}).then(function () { return chrome.tabs.query({}); }).then(function (tabs) { document.title = 'TABS-AFTER:' + tabs.map(function (t) { return t.id + ':' + (t.muted ? 'm' : 's'); }).join('|'); })
wait 800
js document.title
wait 500
quit"

echo "== tabs"
run_demo tabs tabs.script --plugin-dir="$WORK/plugins"
log=$WORK/tabs.log
# Browser 1: primary tab, muted by the script's mute step. Browser 2: the
# new-window, unmuted. Zoom reads 1.5 on both: Chromium stores zoom per host,
# and both pages share the file:// host — the field is per-tab reads of a
# per-host fact. std::map orders ids, so the query lists 1 before 2.
expect 'demo: script [0-9]+ ok value="TABS-OK:1:t:m:1\.5\|2:w:s:1\.5"' "$log" tabs
# tabs.update(1, {muted: false}) goes shim → host → helper mute with a
# browser field — the targeted path, not the active-browser default.
expect 'demo: script [0-9]+ ok value="TABS-AFTER:1:s\|2:s"' "$log" tabs

# ---------------------------------------------------------------- gate 10 ---

# 搭法二验收：popup-mode popover 之后，::plugin-action 的弹出页进挂在
# action 按钮下的 GtkPopover，surface 按清单钳位后的尺寸 configure；
# popover 收起（这里是脚本 quit 前的 browser 关闭路径）走 frames_suspended
# 与 browser-closed 清理。机制的验收，不是库的承诺。
write_script popover.script "popup-mode popover
plugin-action demo
wait 2000
surface-size 2
wait 2000
surface-size 2
wait 2000
surface-size 2
wait 1000
quit"

echo "== popover"
run_demo popover popover.script --plugin-dir="$WORK/plugins"
log=$WORK/popover.log
expect 'demo: popup mode popover' "$log" popover
expect 'demo: browser [0-9]+ created \[popup\] cefnest-extension://demo/popup.html' "$log" popover
expect 'demo: popup popover browser [0-9]+ \(320x480\)' "$log" popover
# The announce line reads the surface before the popover's configure lands;
# the probes read it after — the manifest's clamped popup box. The helper's
# second window maps slowly while the popover's grab is held, so the probe
# runs three times and one hit is the assertion.
expect 'demo: surface-size browser=[0-9]+ size=320x480' "$log" popover

# ---------------------------------------------------------------- gate 11 ---

# 后台页：bgpage 插件声明 background.service_worker，启用后 session 常驻一个
# 隐藏 browser 跑合成页（scheme handler 包装 sw 脚本）。内容脚本
# sendMessage → 后台页 onMessage 收到 → 写 storage；内容脚本轮询 storage 读
# 回——sendMessage 的 echo 与后台页的 storage.set 是两条异步路，轮询把顺序
# 变成确定。断言：往返通、隐藏 browser 不占 FIFO 位、disable 关页、
# enable 重建。
mkdir -p "$WORK/plugins/bgpage"
printf '{"manifest_version": 3, "name": "BG", "version": "1", "permissions": ["storage"], "background": {"service_worker": "bg.js"}, "content_scripts": [{"matches": ["file://*/*plugin-bg-page.html"], "js": ["cs.js"], "run_at": "document_start"}], "cefnest": {"type": "web"}}\n' \
  > "$WORK/plugins/bgpage/cefnest-plugin.json"
cat > "$WORK/plugins/bgpage/bg.js" <<'EOF'
chrome.runtime.onMessage.addListener(function (msg) {
  chrome.storage.local.set({ bg: "heard:" + (msg && msg.n) });
});
EOF
cat > "$WORK/plugins/bgpage/cs.js" <<'EOF'
(function () {
  var deadline = Date.now() + 8000;
  // The send races the background page's own load: a message that arrives
  // before its listener registered is gone. So send again whenever storage
  // still says nothing — the first send that lands after registration is
  // the one the page reads, and that is the path under test either way.
  function send() {
    chrome.runtime.sendMessage({ n: 7 }).then(poll, retry);
  }
  function retry() {
    if (Date.now() < deadline) setTimeout(send, 200);
    else document.title = "BG-SEND-TIMEOUT";
  }
  function poll() {
    chrome.storage.local.get("bg").then(function (v) {
      if (v && v.bg) { document.title = "BG-OK:" + v.bg; return; }
      if (Date.now() < deadline) { setTimeout(send, 200); return; }
      document.title = "BG-TIMEOUT";
    }, function () { document.title = "BG-ERR"; });
  }
  send();
})();
EOF

write_script background.script "load file://$ROOT/test/plugin-bg-page.html
wait 4500
js document.title
wait 500
new-window file://$ROOT/test/plugin-bg-page.html
wait 1500
plugin-disable bgpage
wait 1500
plugin-enable bgpage
wait 2500
quit"

echo "== background"
run_demo background background.script --plugin-dir="$WORK/plugins"
log=$WORK/background.log
expect 'helper: new background page slot' "$log" background
expect 'demo: script [0-9]+ ok value="BG-OK:heard:7"' "$log" background
# The resident page runs in a browser that owns no toplevel: the FIFO the
# demo hands windows out of never hears about it, and the window opened
# after it still pairs with its own surface.
bgid=$(grep -o 'demo: browser [0-9]* created \[background\]' "$log" | head -1 | grep -o '[0-9]\+')
if [ -n "$bgid" ]; then
  ok "background: resident page is browser $bgid"
  expect_no "browser-surface id=$bgid" "$log" background
  # disable tears the page down; enable builds a fresh one under a new id.
  expect "helper: browser $bgid closing" "$log" background
  closes=$(grep -c 'created \[background\]' "$log")
  if [ "$closes" = 2 ]; then ok "background: recreated after re-enable"
  else bad "background: $closes resident pages, 2 wanted"; fi
else
  bad "background: no resident page in the log"
fi

# ---------------------------------------------------------------- gate 12 ---

# Three broken manifests through a real directory get the same machine
# tokens the model tests pin down.
write_script broken.script "load file://$ROOT/test/menu-page.html
wait 1500
quit"

run_demo broken broken.script --plugin-dir="$WORK/broken"
log=$WORK/broken.log
expect 'demo: plugin error badver: bad-manifest' "$log" broken
expect 'demo: plugin error badtype: unsupported-type' "$log" broken
# background.service_worker is acted on now (the resident hidden page), so the
# bgserv manifest scans clean: its row is a plugin, not an error.
expect_no 'plugin error bgserv' "$log" broken

# ---------------------------------------------------------------- gate 7 ----

# Unload safety under fatal-criticals: a stale handler, a double disconnect or
# a callback into an unmapped .so all surface here as a critical — and the
# whole run dies on it.
write_script unload.script "load $PAGE
wait 2500
plugin-disable demo
wait 1000
restart
wait 3000
plugin-enable demo
wait 1000
restart
wait 3000
load $PAGE
wait 2500
click 600 400 273
wait 1500
menu-pick 26600
wait 1000
quit"

echo "== unload"
CHROMIUM_SESSION_SCRIPT=$WORK/unload.script CHROMIUM_HELPER=$HELPER \
  G_DEBUG=fatal-criticals \
  timeout --kill-after=10s 120 "$DEMO" --plugin-dir="$WORK/plugins" </dev/null >"$WORK/unload.log" 2>&1
rc=$?
log=$WORK/unload.log
if [ "$rc" = 0 ]; then ok "unload: demo exited 0 under fatal-criticals"
else bad "unload: demo exited $rc"; tail -20 "$log" | sed 's/^/         /'; fi

# Loads: at the first enable (before any helper exists), at the re-enable
# after the first restart, and at the second restart's ::ready — an enabled
# plugin whose .so ::closed unmapped comes back with the new helper.
# Unloads: on the disable, on the second restart's ::closed, and at dispose —
# each with the full ledger on the line, row count and connection count
# together, which is the accounting closing to zero every time.
loads=$(grep -c 'plugin: native "demo" loading' "$log")
if [ "$loads" = 3 ]; then ok "unload: three loads (enable, re-enable, ready after restart)"
else bad "unload: $loads load lines, 3 wanted"; fi
unloads=$(grep -c 'plugin: native "demo" unloading (1 menu rows, 1 connections)' "$log")
if [ "$unloads" = 3 ]; then ok "unload: three unloads, each closing one row and one connection"
else bad "unload: $unloads unload lines, 3 wanted"; fi
expect 'demo: session restarted' "$log" unload
# After the second restart the row is back: ::ready announced it again.
expect 'helper: plugin menu row slot=0 label=' "$log" unload
helper_rows=$(grep -c 'helper: plugin menu row slot=0 label=' "$log")
if [ "$helper_rows" -ge 2 ]; then ok "unload: the row was re-announced after the restart ($helper_rows announce lines)"
else bad "unload: $helper_rows announce lines, at least 2 wanted"; fi
expect 'demo: menu pick id=26600' "$log" unload
expect 'plugin-demo: menu command slot=0' "$log" unload
# The registry's connection accounting went to zero between the unload and
# the reload — the reload reconnects, which is what the second
# "unloading (1 menu rows, 1 connections)" already counted.
expect_no 'CRITICAL' "$log" unload
expect_no 'Gtk-CRITICAL\|GLib-GObject-CRITICAL\|chromium-plugin-CRITICAL' "$log" unload

echo
if [ "$fails" = 0 ]; then
  printf 'plugin gate: passed\n'
else
  printf 'plugin gate: %d check(s) failed\n' "$fails"
  for f in baseline native escape cs noperm datadir persist nodisk payload broken unload; do
    [ -s "$WORK/$f.log" ] || continue
    echo "-- $f"; tail -40 "$WORK/$f.log" | sed 's/^/  /'
  done
  exit 1
fi
