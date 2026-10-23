#!/usr/bin/env bash
#
# check-cef-pin.sh — the CEF distribution on this machine has to be the one
# scripts/fetch-cef.sh pins.
#
# third_party/ is not in git, so a runner can be prepared once and then drift
# without anything in the tree noticing. The version is in three places:
#
#   PINNED in scripts/fetch-cef.sh                what a fresh fetch downloads
#   third_party/cef/dist/.cef-version             what the last fetch recorded
#   third_party/cef/dist/include/cef_version.h    what the helper compiles against
#
# Comparing only the last two — which is what this check did for a while —
# compares two files the *same* fetch wrote, so it can only notice a half
# replaced dist/, and never that this tree was prepared against a CEF the
# scripts no longer ask for. Both edges are checked here, because a green gate
# that grades a browser nobody pinned is the failure this exists to stop.
#
# Fetched something else on purpose (`CEF_VERSION=latest`, README's
# "pin another build")? That is a deliberate act, so it should be said at the
# gate rather than through a knob here: run the battery with
# CI_SKIP_HELPER_BUILD=1, which gives up the helper rebuild along with this
# check, or fetch the pinned version back.
#
# Run with no arguments from anywhere in the checkout.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
FETCH="$ROOT/scripts/fetch-cef.sh"
HEADER="$ROOT/third_party/cef/dist/include/cef_version.h"
RECORD="$ROOT/third_party/cef/dist/.cef-version"

PIN=$(sed -n "s/^PINNED='\(.*\)'$/\1/p" "$FETCH")
if [ -z "$PIN" ]; then
  # Not a drift report — the check itself lost its subject, most likely because
  # the line in fetch-cef.sh changed shape. Say so instead of passing.
  echo "check-cef-pin: no PINNED='<version>' line in $FETCH" >&2
  exit 1
fi

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

report() {
  echo "check-cef-pin: the CEF distribution is not the one this tree pins" >&2
  echo "  pinned:       $PIN ($FETCH)" >&2
  echo "  fetched:      $VERSION ($RECORD)" >&2
  echo "  on disk:      ${FOUND:-<empty>} ($HEADER)" >&2
  echo "  run ./scripts/fetch-cef.sh with no CEF_VERSION to take the pinned one" >&2
}

if [ "$FOUND" != "$VERSION" ]; then
  # dist/ was written by two different fetches: the record and the headers do
  # not describe the same tree, so nothing built against it is reproducible.
  report
  exit 1
fi

if [ "$VERSION" != "$PIN" ]; then
  report
  exit 1
fi

echo "check-cef-pin: CEF $VERSION matches the pin and the distribution on disk"
