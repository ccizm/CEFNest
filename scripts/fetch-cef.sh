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
# session scripts) and re-measuring docs/internals/protocol-ceilings.md.
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

PINNED='154.0.34+g14c5a08+chromium-154.0.8037.98'
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
URL="https://cef-builds.spotifycdn.com/cef_binary_${ENC}_${ARCH}_minimal.tar.bz2"

if [ "${1:-}" = '--print-url' ]; then
  echo "$URL"
  exit 0
fi

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
curl -fL --retry 3 -o "$DEST/cef.tar.bz2" "$URL"

# The archive has to be the bytes the build service published. A missing
# digest is a failure, not a reason to skip the check.
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

rm -rf "$DEST/dist"
tar -xjf "$DEST/cef.tar.bz2" -C "$DEST"
mv "$DEST"/cef_binary_* "$DEST/dist"
rm -f "$DEST/cef.tar.bz2"

printf '%s\n' "$VERSION" > "$DEST/dist/.cef-version"
printf '%s\n' "$PACKAGE_VERSION" > "$DEST/dist/.cef-package-version"
printf '%s\n' "$CHROMIUM_VERSION" > "$DEST/dist/.chromium-version"

echo "CEF distribution at $DEST/dist (CEF $VERSION, Chromium $CHROMIUM_VERSION)"
echo "helper build picks it up automatically (helper/CMakeLists.txt)."
