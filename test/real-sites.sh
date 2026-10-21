#!/usr/bin/env bash
#
# test/real-sites.sh — real-page inspection batch.
#
# Each site gets its own nested headless session via test/ci-session.sh, so the
# batch uses the same isolation, timeout and cleanup as the release gates without
# putting a window in front of anyone. Artifacts under
#
#   builddir/real-sites/<timestamp>/<site>.{png,session,out,harness.log}
#
# are the point: a screenshot, the JS readback, and a 3-second rAF counter.
# The batch is a report, not a pixel gate: a site can load and answer while the
# video codec is unavailable or WebGL is software-only. Those are findings in
# summary.md, not a reason to hide the other sites.
#
#   ./test/real-sites.sh                 # all sites in test/real-sites.tsv
#   ./test/real-sites.sh docs app        # selected sites
#   REAL_SITES_OUT=/tmp/run ./test/real-sites.sh
#
# It does not run as part of ./test/ci-all.sh by default; the sites are external
# and change under us. CI_ALL_REAL=1 adds it as the `real` line, and it is a
# network-dependent run rather than a release gate.
set -u -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
CI_SESSION="$ROOT/test/ci-session.sh"
CONFIG="$ROOT/test/real-sites.tsv"
OUT=${REAL_SITES_OUT:-"$ROOT/builddir/real-sites/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$OUT"

only=("$@")
wanted() {
  [ ${#only[@]} -eq 0 ] && return 0
  local want=$1 o
  for o in "${only[@]}"; do [ "$o" = "$want" ] && return 0; done
  return 1
}

fails=0
rows=()
summary="$OUT/summary.md"
{
  echo "# Real-site inspection — $(date -Is)"
  echo
  echo "| site | HTTP 200 | ready | screenshot | colors | frames / 3s | JS | note |"
  echo "| --- | --- | --- | --- | --- | --- | --- | --- |"
} >"$summary"

while IFS=$'\t' read -r name url wait_ms; do
  [ -n "${name:-}" ] || continue
  case "$name" in \#*) continue;; esac
  wanted "$name" || continue
  echo
  echo "== $name: $url"
  log="$OUT/$name.harness.log"
  PAYLOAD_TIMEOUT=240 "$CI_SESSION" "$ROOT/test/real-site-run.sh" "$name" "$url" "$OUT" "$wait_ms" >"$log" 2>&1
  rc=$?
  [ -f "$OUT/$name.out" ] || : >"$OUT/$name.out"
  http=0
  grep -qE 'HTTP 200|loaded .* \(HTTP 200\)' "$OUT/$name.out" "$log" 2>/dev/null && http=1
  png="$OUT/$name.png"
  colors=""
  [ -f "$png" ] && colors=$(identify -format '%k' "$png" 2>/dev/null || true)
  js=""
  jsline=$(grep -E 'demo: script [0-9]+ ok value=' "$OUT/$name.out" 2>/dev/null | tail -1 || true)
  if [ -n "$jsline" ]; then
    b64=$(printf '%s\n' "$jsline" | sed -n 's/.* value="\([^"]*\)".*/\1/p')
    [ -n "$b64" ] && js=$(printf '%s' "$b64" | base64 -d 2>/dev/null || true)
  fi
  ready="?"
  frames="?"
  if [ -n "$js" ]; then
    printf '%s\n' "$js" >"$OUT/$name.readback.json"
    ready=$(python3 -c 'import json,sys; print(json.loads(sys.stdin.read()).get("ready","?"))' <<<"$js" 2>/dev/null || echo "?")
    frames=$(python3 -c 'import json,sys; print(json.loads(sys.stdin.read()).get("frames","?"))' <<<"$js" 2>/dev/null || echo "?")
  fi
  note=""
  bad=0
  if [ $rc != 0 ]; then note="demo exit $rc"; bad=1; fi
  if [ ! -f "$png" ]; then note="${note:+$note; }no screenshot"; bad=1; fi
  if [ -n "$colors" ] && [ "$colors" -le 1 ] 2>/dev/null; then note="${note:+$note; }flat screenshot"; bad=1; fi
  if [ -z "$js" ]; then note="${note:+$note; }no JS readback"; bad=1; fi
  if [ "$http" != 1 ]; then note="${note:+$note; }no load-end (ready=$ready)"; fi
  if [ -n "$js" ]; then
    site_note=$(python3 -c 'import json,sys; d=json.loads(sys.stdin.read()); print(", ".join(f"{k}={d[k]}" for k in ("videos","mp4","vp9","videoReady","canvas","webgl","input","article","main","buttons","text") if k in d))' <<<"$js" 2>/dev/null || true)
    [ -n "$site_note" ] && note="${note:+$note; }$site_note"
  fi
  aborts=$(grep -ac 'stack smashing' "$OUT/$name.out" 2>/dev/null || true)
  [ "${aborts:-0}" -gt 0 ] 2>/dev/null && note="${note:+$note; }cef subprocess abort lines=$aborts"
  if [ $bad = 1 ]; then
    printf '  note: %s\n' "$note"
  else
    printf '  ok: screenshot %s colours, %s rAF frames / 3s%s\n' \
      "${colors:-?}" "$frames" "${note:+; $note}"
  fi
  printf '  artifact: %s\n' "$png"
  printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$name" "$([ $http = 1 ] && echo yes || echo no)" "$ready" \
    "$([ -f "$png" ] && echo yes || echo no)" "${colors:-—}" "$frames" \
    "$([ -n "$js" ] && echo yes || echo no)" "${note:-ok}" >>"$summary"
  rows+=("$name|$rc|$http|$colors|$frames|$note")
  [ $bad = 0 ] || fails=$((fails + 1))
done <"$CONFIG"

{
  echo
  echo "Artifacts: \`$OUT\`"
  echo
  echo "Each \`<site>.out\` is the demo transcript; the line"
  echo "\`demo: script N ok value=\"…\"\` is the base64 JSON readback. \`<site>.session\`"
  echo "is exactly what ran. A \`note\` is a finding, not necessarily a gate failure."
} >>"$summary"

echo
echo "== summary: $summary"
cat "$summary"
echo
if [ $fails = 0 ]; then printf 'real sites: all %d site(s) produced load + screenshot + JS\n' "${#rows[@]}"
else printf 'real sites: %d of %d site(s) incomplete — see %s\n' "$fails" "${#rows[@]}" "$summary"; exit 1; fi
