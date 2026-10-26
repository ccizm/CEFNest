#!/usr/bin/env bash
#
# One real-site session, run inside test/ci-session.sh.
#
#   test/real-site-run.sh <name> <url> <out-dir>
#
# It writes the session, the demo transcript, the widget dump and its client
# texture into <out-dir>. The batch harness (test/real-sites.sh) reads those
# artifacts; this script deliberately does not decide whether the site is good.
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/builddir/src/cefnest-demo"
name=${1:?name}
url=${2:?url}
out=${3:?out-dir}
wait_override=${4:-}
mkdir -p "$out"

case "$name" in
  search)
    wait_ms=9000
    extra=',input:!!document.querySelector("input[name=q],input[type=search]"),links:document.links.length'
    ;;
  docs)
    wait_ms=8000
    extra=',article:!!document.querySelector("article,main"),main:!!document.querySelector("main"),h1:document.querySelector("h1")?document.querySelector("h1").innerText:""'
    ;;
  video)
    wait_ms=15000
    extra=',videos:document.querySelectorAll("video").length,mp4:document.createElement("video").canPlayType("video/mp4; codecs=\"avc1.42E01E\""),vp9:document.createElement("video").canPlayType("video/webm; codecs=\"vp9\""),videoReady:(document.querySelector("video")||{}).readyState'
    ;;
  maps)
    wait_ms=12000
    extra=',mapText:(document.body&&document.body.innerText||"").slice(0,160)'
    ;;
  app)
    wait_ms=10000
    extra=',buttons:document.querySelectorAll("button").length,svg:document.querySelectorAll("svg").length'
    ;;
  *)
    wait_ms=8000
    extra=''
    ;;
esac

# real-sites.tsv carries the per-site settle time; keep the case above as the
# fallback for a direct invocation of this script.
[ -n "$wait_override" ] && wait_ms=$wait_override

readback='btoa(unescape(encodeURIComponent(JSON.stringify({title:document.title,ready:document.readyState,url:location.href,text:(document.body&&document.body.innerText||"").length,canvas:document.querySelectorAll("canvas").length,webgl:(()=>{const c=document.querySelector("canvas");if(!c)return false;try{return !!(c.getContext("webgl2")||c.getContext("webgl"))}catch(e){return false}})(),frames:window.__raf,framesMs:Math.round(performance.now()-window.__rafT)'"${extra}"'}))))'

session="$out/$name.session"
cat > "$session" <<SESSION
load $url
wait $wait_ms
js-expr (()=>{window.__raf=0;window.__rafT=performance.now();const f=()=>{window.__raf++;requestAnimationFrame(f)};requestAnimationFrame(f);return "raf"})()
wait 3000
js-expr $readback
wait 1500
js-expr $readback
wait 1500
dump $out/$name.png
quit
SESSION

echo "== real site $name $url"
echo "== session $session"
timeout --kill-after=10s 180 env CHROMIUM_SESSION_SCRIPT="$session" "$DEMO" \
  >"$out/$name.out" 2>"$out/$name.err"
status=$?
cat "$out/$name.err" >>"$out/$name.out"
echo "== demo exit $status"
exit $status
