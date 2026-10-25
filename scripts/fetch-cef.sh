#!/usr/bin/env bash
# Downloads a CEF binary distribution into third_party/cef/dist.
#
# The default is the stable build this tree is verified against, not whatever
# the CEF index happens to call stable today: the index carries the same build
# under more than one channel when a release is promoted, and a new major must
# not move the verified baseline by itself. CEF_VERSION can name another exact
# build, "latest" (or "stable") takes the newest stable entry in the index, and
# CEF_CHANNEL=any takes the first build whatever its channel. The package build
# uses the CEF X.Y.Z part as the CEFNest package version, so the three version
# files written below travel with the distribution and are read by
# scripts/build-packages.sh.
#
# Bumping the build changes what Wayland protocols Chromium asks for, so a new
# CEF milestone still means re-running the whole gate (meson test + audit + the
# session scripts) and re-measuring the protocol ceilings.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/cef"
INDEX_URL='https://cef-builds.spotifycdn.com/index.json'

# CEF's build service names the archive by target architecture, not by the
# build machine. Keep the two supported names explicit; anything else must fail
# instead of silently downloading the wrong libcef.so.
case "${CEF_ARCH:-$(uname -m)}" in
  x86_64|amd64) ARCH='linux64' ;;
  aarch64|arm64) ARCH='linuxarm64' ;;
  *)
    echo "fetch: unsupported architecture '${CEF_ARCH:-$(uname -m)}'; use CEF_ARCH=x86_64 or aarch64" >&2
    exit 1
    ;;
esac

PINNED='156.0.14+g4fa4ab5+chromium-156.0.8078.48'

# What the pinned build's archive has to hash to, per architecture. The .sha256
# the build service publishes is served from the same host over the same channel
# as the archive, so it can only ever say "these bytes are what that host is
# serving today" — a replaced archive and its own digest still agree. Pinning
# the digest here adds the second question: is it the build this repository
# approved. The two values were taken from the build service on 2026-10-25
# (154's on 2026-10-23) and are checked against the pinned version only; that
# also means the first collection trusted the same channel (trust-on-first-use),
# which is the residue this cannot reach.
case "$ARCH" in
  linux64)    PINNED_SHA256='4191ac920a91a052eba04afb6ed830d47aed46a3e246c434a2f062c23588dc8a' ;;
  linuxarm64) PINNED_SHA256='cdfcd5cafaa126bb7e103e3b6fbdef94a39bb791395eb21fe665ebecd008d995' ;;
esac

REQUEST="${CEF_VERSION:-$PINNED}"
if [ "$REQUEST" = 'latest' ] || [ "$REQUEST" = 'stable' ]; then
  CHANNEL="${CEF_CHANNEL:-stable}"
  resolved=$(
    curl -fsSL --retry 3 "$INDEX_URL" |
      python3 -c '
import json
import sys

data = json.load(sys.stdin)
channel = sys.argv[1]
arch = sys.argv[2]
for item in data.get(arch, {}).get("versions", []):
    if channel in ("any", "latest") or item.get("channel") == channel:
        print(item["cef_version"])
        print(item["chromium_version"])
        break
else:
    raise SystemExit("fetch: no %s build for %s" % (channel, arch))
' "$CHANNEL" "$ARCH"
  )
  VERSION=$(printf '%s\n' "$resolved" | sed -n '1p')
  CHROMIUM_VERSION=$(printf '%s\n' "$resolved" | sed -n '2p')
else
  VERSION="$REQUEST"
  CHROMIUM_VERSION="${CEF_CHROMIUM_VERSION:-}"
  if [ -z "$CHROMIUM_VERSION" ]; then
    CHROMIUM_VERSION=$(printf '%s\n' "$VERSION" |
      sed -n 's/.*chromium-\([0-9][0-9.]*\).*/\1/p')
  fi
fi

[ -n "$VERSION" ] || { echo "fetch: empty CEF version" >&2; exit 1; }
[ -n "$CHROMIUM_VERSION" ] || {
  echo "fetch: could not determine the Chromium version for $VERSION" >&2
  echo "  name it with CEF_CHROMIUM_VERSION=..." >&2
  exit 1
}
PACKAGE_VERSION=$(printf '%s\n' "$VERSION" |
  sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$PACKAGE_VERSION" ] || {
  echo "fetch: CEF version '$VERSION' does not start with X.Y.Z" >&2
  exit 1
}

ENC="${VERSION//+/%2B}"
ARCHIVE_NAME="cef_binary_${ENC}_${ARCH}_minimal.tar.bz2"
URL="https://cef-builds.spotifycdn.com/$ARCHIVE_NAME"

if [ "${1:-}" = '--print-url' ]; then
  echo "$URL"
  exit 0
fi

# The archive is ~311 MB and the route to the build service can be lossy: a
# release build died on curl 18 (partial file) with no way to continue where
# the transfer stopped. download() tries its candidates in order — the
# CEF_MIRROR_URLS mirrors first, then the official service — and gives each
# candidate up to CEF_DOWNLOAD_PASSES passes (default 6). Every pass resumes
# from the bytes already on disk; the CDN serves range requests, and a
# candidate that does not has its partial bytes dropped so the next pass
# starts clean. A pass that stalls below 10 KB/s for 45 s is cut and retried;
# --retry 3 covers the transient connect/5xx class on its own. Whatever the
# source, the pinned digest below still decides whether the bytes are the
# build this repository approved.
download () {
  local dest="$1"
  shift
  local url pass rc ranges
  for url in "$@"; do
    ranges=$(curl -fsIm 20 "$url" | tr -d '\r' | grep -i '^accept-ranges:' || true)
    case "$ranges" in
      *bytes*) : ;;
      *) rm -f "$dest" ;;
    esac
    for pass in $(seq 1 "${CEF_DOWNLOAD_PASSES:-6}"); do
      echo "fetch: $url (pass $pass)"
      rc=0
      curl -fL -C - --retry 3 --retry-all-errors --retry-delay 5 \
        --connect-timeout 20 --speed-time 45 --speed-limit 10240 \
        -o "$dest" "$url" || rc=$?
      if [ "$rc" -eq 0 ]; then
        return 0
      fi
      echo "fetch: curl exited $rc, $(stat -c%s "$dest" 2>/dev/null || echo 0) bytes on disk" >&2
      if [ "$rc" -eq 33 ]; then
        echo "fetch: $url does not serve ranges; restarting that candidate from zero" >&2
        rm -f "$dest"
      fi
    done
  done
  return 1
}

CANDIDATES=()
for MIRROR in ${CEF_MIRROR_URLS:-}; do
  CANDIDATES+=("${MIRROR%/}/$ARCHIVE_NAME")
done
CANDIDATES+=("$URL")

mkdir -p "$DEST"

if [ -f "$DEST/dist/.cef-version" ] &&
   [ "$(cat "$DEST/dist/.cef-version")" = "$VERSION" ] &&
   [ -f "$DEST/dist/Release/libcef.so" ]; then
  printf '%s\n' "$PACKAGE_VERSION" > "$DEST/dist/.cef-package-version"
  printf '%s\n' "$CHROMIUM_VERSION" > "$DEST/dist/.chromium-version"
  echo "fetch: cached $VERSION"
  exit 0
fi

echo "fetch: $VERSION (Chromium $CHROMIUM_VERSION)"
echo "fetch: $URL"
if ! download "$DEST/cef.tar.bz2" "${CANDIDATES[@]}"; then
  echo "fetch: every source gave up on $ARCHIVE_NAME; partial bytes kept at" >&2
  echo "  $DEST/cef.tar.bz2 — rerun to resume where this stopped" >&2
  exit 1
fi

# The archive has to be the bytes the build service published. A missing
# digest is a failure, not a reason to skip the check. This one answers
# "did the transfer survive intact"; it cannot answer "is this the build we
# approved", because the digest comes off the same host over the same channel.
# The pinned digest below does.
echo "fetch: $URL.sha256"
curl -fL --retry 3 -o "$DEST/cef.tar.bz2.sha256" "$URL.sha256"
# The published file starts with a blank line; take the first full digest.
expected=$(tr -d '\r' < "$DEST/cef.tar.bz2.sha256" |
           grep -oE '^[0-9a-fA-F]{64}' | head -1 || true)
actual=$(sha256sum "$DEST/cef.tar.bz2" | cut -d' ' -f1)
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
  echo "fetch: checksum mismatch for $DEST/cef.tar.bz2" >&2
  echo "  expected: ${expected:-<empty>}" >&2
  echo "  actual:   $actual" >&2
  rm -f "$DEST/cef.tar.bz2" "$DEST/cef.tar.bz2.sha256"
  exit 1
fi
rm -f "$DEST/cef.tar.bz2.sha256"

if [ "$VERSION" = "$PINNED" ] && [ "$actual" != "$PINNED_SHA256" ]; then
  echo "fetch: the archive does not hash to the digest pinned for $PINNED" >&2
  echo "  pinned: $PINNED_SHA256" >&2
  echo "  actual: $actual" >&2
  echo "  the build service is serving bytes this repository did not approve, and" >&2
  echo "  unpacking them is exactly what the pin is here to stop. Bump the digest" >&2
  echo "  only after establishing why it moved." >&2
  rm -f "$DEST/cef.tar.bz2"
  exit 1
fi
if [ "$VERSION" != "$PINNED" ]; then
  echo "fetch: note: $VERSION is not the pinned build, so the in-repo digest did"
  echo "  not apply — only the build service's own .sha256 was checked."
fi

rm -rf "$DEST/dist"
tar -xjf "$DEST/cef.tar.bz2" -C "$DEST"
mv "$DEST"/cef_binary_* "$DEST/dist"
rm -f "$DEST/cef.tar.bz2"

printf '%s\n' "$VERSION" > "$DEST/dist/.cef-version"
printf '%s\n' "$PACKAGE_VERSION" > "$DEST/dist/.cef-package-version"
printf '%s\n' "$CHROMIUM_VERSION" > "$DEST/dist/.chromium-version"

echo "CEF distribution at $DEST/dist (CEF $VERSION, Chromium $CHROMIUM_VERSION)"
echo "helper build picks it up automatically (helper/CMakeLists.txt)."
