#!/usr/bin/env bash
#
# test/filter-gate.sh — the declarative response filter, for real.
#
# One local server, one probe page, five scenarios:
#
#   replace    a rule rewrites the page's marker string on the wire; the DOM
#              text the page presents is the verdict
#   spans      the same rule over a generated ~380 KiB page with the marker
#              at several offsets, deep ones included — the engine's chunk
#              stitching is what decides, and the helper's measured line
#              (chunks / in / out / spans) lands in the transcript
#   remove     filter-remove takes the rule back and a reload shows the
#              marker again — no rule, no rewrite
#   css        inject-css lands a <style> before </head>; the computed
#              background color is the verdict
#   header     drop-header is refused by name (filter-add-drop-header-
#              unsupported) and registers nothing — the plan's third op was
#              measured out of existence: CEF 156 marks the response
#              unmodifiable at every callback that carries one, and header
#              rewrites at both OnResourceResponse and GetResourceResponse-
#              Filter never reached the wire
#
#   ./test/filter-gate.sh
#   ./test/ci-session.sh ./test/filter-gate.sh
#
# Needs python3; 77 without it.

set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
. "$ROOT/test/helper-path.sh"

WORK=$(mktemp -d)
SERVER=
KEEP=${KEEP_LOGS:-0}
cleanup() {
  [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null
  if [ "$KEEP" = 1 ]; then
    echo "  note: transcripts kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
expect() { # expect <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then ok "$3: /$1/"; else
    bad "$3: wanted /$1/ — see $2"
    grep -nE 'demo: filter|helper: filter|helper: get-resource|helper status' "$2" | tail -8 | sed 's/^/         /'
  fi
}
expect_no() { # expect_no <pattern> <logfile> <tag>
  if grep -qE "$1" "$2"; then
    bad "$3: must not contain /$1/"
    grep -nE "$1" "$2" | head -4 | sed 's/^/         /'
  else ok "$3: no /$1/"; fi
}

if [ ! -x "$DEMO" ]; then echo "no demo at $DEMO — run ninja -C builddir"; exit 1; fi
if [ ! -e "$HELPER" ]; then echo "no helper at $HELPER"; exit 1; fi
if ! command -v python3 >/dev/null; then echo "no python3 — the probe origin needs an HTTP server"; exit 77; fi

# b64 <json> — the ops ride base64 JSON into the script step, the same
# escape script-add uses for code.
b64() { printf '%s' "$1" | base64 -w0; }

REPLACE_OPS=$(b64 '[{"type":"replace","find":"MARKER-FIND-XYZ","replace":"REPLACED-BY-FILTER"}]')
CSS_OPS=$(b64 '[{"type":"inject-css","css":"body{background-color:rgb(1,2,3)}"}]')
DROP_OPS=$(b64 '[{"type":"drop-header","name":"x-echo-marker"}]')

# The server serves the probe page (with the echo header) and a generated
# ~380 KiB page whose markers sit at depths no first-chunk filter could
# reach. Everything is no-store so a reload after filter-remove really
# asks the network again.
cp "$ROOT/test/filter-page.html" "$WORK/filter-page.html"
cat >"$WORK/server.py" <<'PYEOF'
import http.server, os, sys

ROOT = sys.argv[1]
PORT = int(sys.argv[2])
MARKER = b"MARKER-FIND-XYZ"
FILL = b"<p>filler " + b"x" * 120 + b"</p>\n"

parts = [b"<!doctype html><html><head><title>big</title></head><body>\n"]
target = 380 * 1024
size = 0
depth = 0
markers = 0
while size < target:
    parts.append(FILL)
    size += len(parts[-1])
    depth += 1
    if depth % 700 == 0:
        parts.append(b'<div id="m">' + MARKER + b"</div>\n")
        size += len(parts[-1])
        markers += 1
# The checker builds the marker at runtime: a literal here would be rewrite
# fodder for the very filter under test.
parts.append(b'<script>var m = "MARKER-FIND-" + "XYZ";'
             b'var n = document.body.innerText.split(m).length - 1;'
             b'var r = document.body.innerText.split("REPLACED-BY-FILTER")'
             b'.length - 1;'
             b'document.title = (n === 0 && r === ' + str(markers).encode() +
             b') ? "ALL-REPLACED" : "BAD-n" + n + "-r" + r;'
             b"</script></body></html>\n")
BIG = b"".join(parts)

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = BIG if "probe=big" in self.path else open(
            os.path.join(ROOT, "filter-page.html"), "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("x-echo-marker", self.headers.get("x-marker", "none"))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
PYEOF

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
python3 "$WORK/server.py" "$WORK" "$PORT" >"$WORK/http.log" 2>&1 &
SERVER=$!
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
  sleep 0.1
done

ORIGIN="http://127.0.0.1:$PORT"
PAGE="$ORIGIN/filter-page.html"

run_demo() { # run_demo <tag> <script-file>
  timeout --kill-after=10s 90 env GDK_BACKEND=wayland \
    CHROMIUM_HELPER="$HELPER" CHROMIUM_SESSION_SCRIPT="$2" \
    "$DEMO" >"$WORK/$1.log" 2>&1
}

# --- replace -----------------------------------------------------------------

cat >"$WORK/replace.session" <<EOF
filter-add 1 *://*/filter-page.html* $REPLACE_OPS
load $PAGE?probe=page
wait 2500
js document.getElementById("m").innerText
wait 600
quit
EOF

echo "== filter over $ORIGIN"
run_demo replace "$WORK/replace.session"
log=$WORK/replace.log
if [ "$?" = 0 ]; then ok "replace: demo exited 0"
else bad "replace: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: filter-add 1 \(1 ops\)' "$log" replace
expect 'helper: filter-add 1 rules=1' "$log" replace
expect 'demo: script [0-9]+ ok value="REPLACED-BY-FILTER"' "$log" replace

# --- spans: the same rule over a many-chunk page ------------------------------

cat >"$WORK/spans.session" <<EOF
filter-add 1 *://*/filter-page.html* $REPLACE_OPS
load $PAGE?probe=big
wait 3500
js document.title
wait 600
quit
EOF

run_demo spans "$WORK/spans.session"
log=$WORK/spans.log
if [ "$?" = 0 ]; then ok "spans: demo exited 0"
else bad "spans: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: script [0-9]+ ok value="ALL-REPLACED"' "$log" spans
expect 'helper: filter done chunks=[0-9]+ in=[0-9]{6,}' "$log" spans

# --- remove: the rule goes away, the marker comes back ------------------------

cat >"$WORK/remove.session" <<EOF
filter-add 1 *://*/filter-page.html* $REPLACE_OPS
load $PAGE?probe=page
wait 2500
js document.getElementById("m").innerText
wait 600
filter-remove 1
reload
wait 2500
js document.getElementById("m").innerText
wait 600
quit
EOF

run_demo remove "$WORK/remove.session"
log=$WORK/remove.log
if [ "$?" = 0 ]; then ok "remove: demo exited 0"
else bad "remove: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'helper: filter-remove 1 rules=0' "$log" remove
# First readback replaced, the one after remove+reload back to the marker.
expect 'demo: script [0-9]+ ok value="REPLACED-BY-FILTER"' "$log" remove
expect 'demo: script [0-9]+ ok value="MARKER-FIND-XYZ"' "$log" remove

# --- css: inject-css before </head> -------------------------------------------

cat >"$WORK/css.session" <<EOF
filter-add 1 *://*/filter-page.html* $CSS_OPS
load $PAGE?probe=css
wait 2500
js getComputedStyle(document.body).backgroundColor
wait 600
quit
EOF

run_demo css "$WORK/css.session"
log=$WORK/css.log
if [ "$?" = 0 ]; then ok "css: demo exited 0"
else bad "css: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'demo: script [0-9]+ ok value="rgb\(1, 2, 3\)"' "$log" css

# --- header: drop-header refused by name ---------------------------------------

cat >"$WORK/header.session" <<EOF
filter-add 1 *://*/filter-page.html* $DROP_OPS
load $PAGE?probe=page
wait 2000
js document.title
wait 600
quit
EOF

run_demo header "$WORK/header.session"
log=$WORK/header.log
if [ "$?" = 0 ]; then ok "header: demo exited 0"
else bad "header: demo exited"; tail -8 "$log" | sed 's/^/         /'; fi
expect 'helper status: filter-add-drop-header-unsupported' "$log" header
expect_no 'helper: filter-add 1 rules=' "$log" header

if [ "$fails" = 0 ]; then printf 'filter gate: passed\n'
else printf 'filter gate: %d check(s) failed\n' "$fails"; exit 1; fi
