#!/usr/bin/env bash
# Downloads the pinned CEF binary distribution into third_party/cef/dist.
#
# The version is pinned to the build the helper is developed and verified
# against. Bumping it means: update VERSION, rebuild the helper, and re-run
# the whole gate (meson test + audit + the session scripts) — Chromium
# milestone jumps also change what Wayland protocols the client asks for,
# so docs/internals/protocol-ceilings.md gets re-measured too.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION='154.0.34+g14c5a08+chromium-154.0.8037.98'
DEST="$ROOT/third_party/cef"

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

ENC="${VERSION//+/%2B}"
URL="https://cef-builds.spotifycdn.com/cef_binary_${ENC}_${ARCH}_minimal.tar.bz2"

if [ "${1:-}" = '--print-url' ]; then
  echo "$URL"
  exit 0
fi

mkdir -p "$DEST"
echo "fetch: $URL"
curl -fL --retry 3 -o "$DEST/cef.tar.bz2" "$URL"

rm -rf "$DEST/dist"
tar -xjf "$DEST/cef.tar.bz2" -C "$DEST"
mv "$DEST"/cef_binary_* "$DEST/dist"
rm -f "$DEST/cef.tar.bz2"

echo "CEF distribution at $DEST/dist"
echo "helper build picks it up automatically (helper/CMakeLists.txt)."
