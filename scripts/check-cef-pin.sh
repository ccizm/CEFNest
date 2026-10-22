#!/usr/bin/env bash
#
# check-cef-pin.sh — the CEF distribution on this machine has to be the one
# the build was fetched against.
#
# third_party/ is not in git, so a runner can be prepared once and then drift
# without anything in the tree noticing. fetch-cef.sh records the version it
# downloaded in third_party/cef/dist/.cef-version; include/cef_version.h is
# what the helper actually compiles against. This refuses a run where they
# disagree, instead of letting a green gate grade a browser nobody asked for.
#
# Run with no arguments from anywhere in the checkout.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
HEADER="$ROOT/third_party/cef/dist/include/cef_version.h"
RECORD="$ROOT/third_party/cef/dist/.cef-version"

if [ ! -f "$HEADER" ]; then
  echo "check-cef-pin: no CEF distribution at $HEADER" >&2
  echo "  run ./scripts/fetch-cef.sh first" >&2
  exit 1
fi

if [ ! -f "$RECORD" ]; then
  echo "check-cef-pin: no fetched version recorded at $RECORD" >&2
  echo "  run ./scripts/fetch-cef.sh first (it writes this file)" >&2
  exit 1
fi

VERSION=$(tr -d '\r' < "$RECORD" | head -1)
if [ -z "$VERSION" ]; then
  echo "check-cef-pin: $RECORD is empty" >&2
  exit 1
fi

FOUND=$(sed -n 's/^#define[[:space:]]\+CEF_VERSION[[:space:]]\+"\(.*\)".*$/\1/p' "$HEADER")

if [ "$FOUND" != "$VERSION" ]; then
  echo "check-cef-pin: the CEF distribution does not match the fetched version" >&2
  echo "  recorded: ${VERSION:-<empty>} ($RECORD)" >&2
  echo "  header:   ${FOUND:-<empty>} ($HEADER)" >&2
  exit 1
fi

echo "check-cef-pin: CEF $VERSION matches the distribution on disk"
