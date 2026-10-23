#!/bin/sh
# check-release-tag.sh — the release strings the pipeline publishes have to agree
# before anything is built.
#
# A tag push runs a ~90 minute build and then creates the Release. Until now the
# only thing that could notice a wrong version was the attachment upload: the
# names in .cnb.yml are static (that step cannot expand shell variables), so a
# tag pushed without bumping CEFNEST_PACKAGE_RELEASE — or a bump that forgot one
# of the four versioned asset names — spends the full 90 minutes and then fails
# on a missing file, or worse, publishes a Release whose title and whose package
# versions describe different builds.
#
# What is compared:
#
#   PINNED in scripts/fetch-cef.sh      the X.Y.Z every artifact is named after
#   CEFNEST_PACKAGE_RELEASE             the revision, from the pipeline
#   CEFNEST_EXPECTED_TAG                the tag the pipeline was pushed for,
#                                       written next to the asset names it checks
#   the attachment list in .cnb.yml     every name that carries a version
#
# The tag itself is only checked when the runner has it: `git tag --points-at
# HEAD` answers on a checkout that fetched the tag ref, and CNB has not promised
# one or the other. That is why CEFNEST_EXPECTED_TAG is written in .cnb.yml by
# hand next to the asset names — the same list, the same drift it guards against.
#
# Run from anywhere in the checkout:
#
#   CEFNEST_PACKAGE_RELEASE=8 CEFNEST_EXPECTED_TAG=v154.0.34-8 \
#     ./scripts/check-release-tag.sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

fail() {
  echo "check-release-tag: $*" >&2
  exit 1
}

PINNED=$(sed -n "s/^PINNED='\(.*\)'$/\1/p" scripts/fetch-cef.sh)
[ -n "$PINNED" ] || fail "no PINNED='<version>' line in scripts/fetch-cef.sh"

# meson's library version and both package managers take only X.Y.Z of CEF's
# 154.0.34+g14c5a08+chromium-154.0.8037.98 — the same cut build-packages.sh makes.
VERSION=$(printf '%s\n' "$PINNED" |
          sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
[ -n "$VERSION" ] || fail "could not read an X.Y.Z out of PINNED='$PINNED'"

RELEASE=${CEFNEST_PACKAGE_RELEASE:-1}
EXPECTED_TAG="v$VERSION-$RELEASE"
echo "check-release-tag: artifacts will be named $VERSION-$RELEASE"

if [ -n "${CEFNEST_EXPECTED_TAG:-}" ] &&
   [ "$CEFNEST_EXPECTED_TAG" != "$EXPECTED_TAG" ]; then
  fail "$CEFNEST_EXPECTED_TAG is not $EXPECTED_TAG — the tag, CEFNEST_PACKAGE_RELEASE and the names in .cnb.yml have to move together"
fi

# The runner may or may not hand us the tag ref; when it does, use it, since that
# is the only way to check the thing a human actually typed.
GIT_TAGS=$(git tag --points-at HEAD 2>/dev/null || true)
case "$GIT_TAGS" in
  "") echo "check-release-tag: no tag on HEAD in this checkout, relying on CEFNEST_EXPECTED_TAG" ;;
  *)  for tag in $GIT_TAGS; do
        [ "$tag" = "$EXPECTED_TAG" ] ||
          fail "HEAD carries tag $tag, which is not $EXPECTED_TAG"
      done
      echo "check-release-tag: tag on HEAD is $GIT_TAGS" ;;
esac

# Every attachment name that is supposed to carry the version has to carry this
# one. cefnest-build.tar.gz and SHA256SUMS are the two names the pipeline uploads
# that never get a version string, so they are the two exempted by name.
names=$(sed -n 's/^[[:space:]]*-[[:space:]]*"\.\/\([^"]*\)"$/\1/p' .cnb.yml)
[ -n "$names" ] || fail "no attachment entries found in .cnb.yml — the check lost its subject"
checked=0
for name in $names; do
  case "$name" in
    cefnest-build.tar.gz|*/cefnest-build.tar.gz|SHA256SUMS|*/SHA256SUMS) continue ;;
  esac
  checked=$((checked + 1))
  case "$name" in
    *"$VERSION-$RELEASE"*) ;;
    *) fail "the attachment list uploads $name, which is not a $VERSION-$RELEASE file" ;;
  esac
done
echo "check-release-tag: $checked versioned attachment name(s) agree"
echo "check-release-tag: ok"
