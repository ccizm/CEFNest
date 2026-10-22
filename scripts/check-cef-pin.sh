#!/usr/bin/env bash
#
# check-cef-pin.sh — the CEF distribution on this machine has to be the one
# scripts/fetch-cef.sh pins.
#
# third_party/ is not in git, so a runner can be prepared once and then drift
# from the pin without anything in the tree noticing. The version is in two
# places: the VERSION line in fetch-cef.sh (what a fresh fetch downloads) and
# CEF_VERSION in third_party/cef/dist/include/cef_version.h (what the build
# actually compiles against). This refuses a run where they disagree, instead
# of letting a green gate grade a browser nobody asked for.
#
# Run with no arguments from anywhere in the checkout.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
FETCH="$ROOT/scripts/fetch-cef.sh"
HEADER="$ROOT/third_party/cef/dist/include/cef_version.h"

VERSION=$(sed -n "s/^VERSION='\(.*\)'$/\1/p" "$FETCH")

if [ -z "$VERSION" ]; then
  echo "check-cef-pin: no VERSION= line in $FETCH" >&2
  exit 1
fi

if [ ! -f "$HEADER" ]; then
  echo "check-cef-pin: no CEF distribution at $HEADER" >&2
  echo "  run ./scripts/fetch-cef.sh first" >&2
  exit 1
fi

FOUND=$(sed -n 's/^#define[[:space:]]\+CEF_VERSION[[:space:]]\+"\(.*\)".*$/\1/p' "$HEADER")

if [ "$FOUND" != "$VERSION" ]; then
  echo "check-cef-pin: the CEF distribution does not match the pin" >&2
  echo "  pinned:  ${VERSION:-<empty>}" >&2
  echo "  on disk: ${FOUND:-<empty>} ($HEADER)" >&2
  exit 1
fi

echo "check-cef-pin: CEF $VERSION matches the distribution on disk"
