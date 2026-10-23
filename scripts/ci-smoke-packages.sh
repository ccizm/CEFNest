#!/bin/sh
# ci-smoke-packages.sh — install the packages the way a user does, then check
# what a user would find.
#
# The tag pipeline used to build the packages and stop there: nothing proved the
# four deb Depends strings (hand-written, in scripts/build-packages.sh) name
# libraries that exist, that the CEF runtime really travels next to the helper,
# or that the 1.5 GB of files satisfy each other's loader. All three are the
# kind of thing a user finds out by having nothing on screen.
#
# The sandbox question this also answers: chrome-sandbox ships 0755 and the
# helper starts with the sandbox on, so a host that restricts unprivileged user
# namespaces (Ubuntu 24.04 turns apparmor_restrict_unprivileged_userns on by
# default) refuses to run the package at all with "No usable sandbox!". A
# container cannot reproduce somebody else's AppArmor policy, so this does not
# pretend to test that host. What it does test is the *packaged* default on this
# host: the helper is started the way a user would start it and the transcript
# is graded — a sandbox FATAL here is a real regression, and the two documented
# ways out (a per-application AppArmor profile, or CHROMIUM_HELPER_NO_SANDBOX=1)
# are reported side by side. docs/internals/sandbox-policy.md §4 is the
# measurement this shape comes from.
#
# Run after scripts/ci-build.sh, in the same workspace, as root:
#
#   ./scripts/ci-smoke-packages.sh
#
# Not root? The inspection half still runs and the install half is skipped out
# loud — CI has to say so, so CI fails unless the caller says the skip is meant:
#
#   CI_SMOKE_SKIP_INSTALL=1 ./scripts/ci-smoke-packages.sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

DIST="$ROOT/dist"
ARCH=$(uname -m)
HELPER_DIR=/usr/libexec/cefnest/$ARCH
CEF_VERSION=$(cat third_party/cef/dist/.cef-version)
CHROMIUM_VERSION=$(cat third_party/cef/dist/.chromium-version)
VERSION=$(cat third_party/cef/dist/.cef-package-version)
RELEASE=${CEFNEST_PACKAGE_RELEASE:-1}

fails=0
ok() { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
note() { printf '  ..   %s\n' "$1"; }

[ -d "$DIST" ] || { echo "smoke: no $DIST — run scripts/ci-build.sh first" >&2; exit 1; }

AS_ROOT=1
if [ "$(id -u)" != 0 ]; then
  AS_ROOT=0
fi
if [ "$AS_ROOT" != 1 ] && [ "${CI_SMOKE_SKIP_INSTALL:-0}" != 1 ]; then
  echo "smoke: not root, so the install half cannot run. Re-run as root, or say" >&2
  echo "  you meant the reduced run with CI_SMOKE_SKIP_INSTALL=1." >&2
  exit 1
fi
if [ "$AS_ROOT" != 1 ]; then
  note "not root: skipping every step that installs or runs an installed helper"
fi

echo "== smoke: the manifest covers what the pipeline uploads"
# The build tarball is hashed here too — it is moved into dist/ by
# build-packages.sh for exactly this reason.
if (cd "$DIST" && sha256sum -c SHA256SUMS >/dev/null); then
  if grep -q cefnest-build.tar.gz "$DIST/SHA256SUMS"; then
    ok "SHA256SUMS verifies, and lists cefnest-build.tar.gz"
  else
    bad "SHA256SUMS verifies but does not cover cefnest-build.tar.gz"
  fi
else
  bad "sha256sum -c failed in $DIST"
  (cd "$DIST" && sha256sum -c SHA256SUMS 2>&1 | tail -12) | sed 's/^/         /'
fi

echo "== smoke: package contents"
# dpkg -c needs no root and no install: it reads the archive. Checking the names
# inside the .deb is what catches "the helper is there but its runtime is not"
# before anything tries to load it.
for pkg in libcefnest0 libcefnest-dev cefnest-demo cefnest-helper; do
  deb="$DIST/${pkg}_${VERSION}-${RELEASE}_amd64.deb"
  [ -f "$deb" ] || { bad "no $deb"; continue; }
  ok "present: $deb"
done

HELPER_LIST=$(dpkg -c "$DIST/cefnest-helper_${VERSION}-${RELEASE}_amd64.deb" 2>/dev/null || true)
for needed in "$HELPER_DIR/cefnest-helper" "$HELPER_DIR/libcef.so" \
              "$HELPER_DIR/icudtl.dat" "$HELPER_DIR/resources.pak" \
              "$HELPER_DIR/v8_context_snapshot.bin" "$HELPER_DIR/chrome-sandbox" \
              "$HELPER_DIR/locales"; do
  case "$HELPER_LIST" in
    *"$needed"*) ok "cefnest-helper carries $needed" ;;
    *) bad "cefnest-helper does not carry $needed" ;;
  esac
done

LIB_LIST=$(dpkg -c "$DIST/libcefnest0_${VERSION}-${RELEASE}_amd64.deb" 2>/dev/null || true)
case "$LIB_LIST" in
  *"libcefnest.so.$VERSION"*) ok "libcefnest0 carries the versioned object" ;;
  *) bad "libcefnest0 has no libcefnest.so.$VERSION — check -Dcefnest-version" ;;
esac

# chrome-sandbox is deliberately 0755: this project ships no setuid helper and
# no AppArmor profile it has not measured (docs/internals/sandbox-policy.md §6).
# The mode is asserted so a future packaging change cannot quietly turn it into
# a privilege-escalation surface nobody reviewed.
SBX_MODE=$(dpkg -c "$DIST/cefnest-helper_${VERSION}-${RELEASE}_amd64.deb" 2>/dev/null |
  awk -v p="$HELPER_DIR/chrome-sandbox" '$0 ~ p { print substr($1, 1, 10); exit }')
if [ "$SBX_MODE" = "-rwxr-xr-x" ]; then
  ok "chrome-sandbox is 0755 (not setuid, as decided)"
else
  bad "chrome-sandbox mode reads as '${SBX_MODE:-unknown}', expected -rwxr-xr-x"
fi

# Policy §12.5: a binary package's copyright file lives in the data tree, at
# /usr/share/doc/<package>/copyright. This is checked where it is read from —
# the first version wrote DEBIAN/copyright instead, dpkg never installs that
# member, and lintian still reported E: no-copyright-file. Reading the member
# out of the archive costs one pass over the helper's 1.5 GB and runs without
# root, so the reduced CI_SMOKE_SKIP_INSTALL=1 pass still checks it.
for pkg in libcefnest0 libcefnest-dev cefnest-demo cefnest-helper; do
  deb="$DIST/${pkg}_${VERSION}-${RELEASE}_amd64.deb"
  [ -f "$deb" ] || { bad "no $deb to read the copyright from"; continue; }
  dep5=$(dpkg-deb --fsys-tarfile "$deb" 2>/dev/null |
         tar -xO "./usr/share/doc/$pkg/copyright" 2>/dev/null || true)
  case "$dep5" in
    *copyright-format/1.0*License:\ LGPL-2.1*)
      ok "$pkg ships /usr/share/doc/$pkg/copyright" ;;
    *) bad "$pkg has no usable /usr/share/doc/$pkg/copyright" ;;
  esac
  if [ "$pkg" = cefnest-helper ]; then
    case "$dep5" in
      *"Files: usr/libexec/cefnest/"*BSD-3-clause*)
        ok "cefnest-helper records the CEF runtime as BSD-3-clause" ;;
      *) bad "cefnest-helper does not record a license for the CEF runtime it carries" ;;
    esac
  fi
done

if ls "$DIST"/*.rpm >/dev/null 2>&1; then
  echo "== smoke: rpm contents (listed, not installed — this is a Debian host)"
  command -v rpm >/dev/null 2>&1 || { note "no rpm tool: skipping the rpm listing"; }
  if command -v rpm >/dev/null 2>&1; then
    for rpm_name in "cefnest-${VERSION}-${RELEASE}.$ARCH.rpm" \
                    "cefnest-devel-${VERSION}-${RELEASE}.$ARCH.rpm" \
                    "cefnest-demo-${VERSION}-${RELEASE}.$ARCH.rpm" \
                    "cefnest-helper-${VERSION}-${RELEASE}.$ARCH.rpm"; do
      [ -f "$DIST/$rpm_name" ] || { bad "no $DIST/$rpm_name"; continue; }
      if rpm -qp --requires "$DIST/$rpm_name" >/dev/null 2>&1; then
        ok "rpm -qp readable: $rpm_name"
      else
        bad "rpm -qp failed on $rpm_name"
      fi
    done
    case "$(rpm -qlp "$DIST/cefnest-helper-${VERSION}-${RELEASE}.$ARCH.rpm" 2>/dev/null || true)" in
      *"$HELPER_DIR/libcef.so"*) ok "the helper rpm carries the CEF runtime too" ;;
      *) bad "the helper rpm does not list $HELPER_DIR/libcef.so" ;;
    esac
    # Reported, not asserted: the spec's own Requires for the helper is one line
    # (glibc), so whether a host is told about libnss3/libgbm and the rest comes
    # from rpmbuild's own soname discovery — which %global __os_install_post
    # %{nil} does *not* cover (that macro stops the brp post-processing, not
    # find-requires). Nobody in this repo has an rpm host to have measured it
    # on, so the line below is the measurement, and docs/testing/cnb-ci.md is
    # meant to be updated from what a real run prints.
    note "cefnest rpm --requires: $(rpm -qp --requires \
      "$DIST/cefnest-helper-${VERSION}-${RELEASE}.$ARCH.rpm" 2>/dev/null |
      tr '\n' ' ')"
  fi
fi

if [ "$AS_ROOT" = 1 ]; then
  echo "== smoke: apt installs these Depends"
  # This is the check the hand-written Depends strings have never had: apt
  # resolves each name against the archive and refuses the install if one is
  # wrong, invented, or older than the library needs. tzdata's postinst is the
  # same bind-mounted /etc/localtime problem ci-build.sh works around, so it is
  # worked around here too.
  export DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC
  if command -v debconf-set-selections >/dev/null 2>&1; then
    printf 'tzdata tzdata/Areas select Etc\ntzdata tzdata/Zones/Etc select UTC\n' |
      debconf-set-selections
  fi
  if awk '$2 == "/etc/localtime" { found = 1 } END { exit !found }' \
       /proc/mounts 2>/dev/null; then
    umount /etc/localtime 2>/dev/null || true
  fi
  apt-get update -qq
  if apt-get install -y --no-install-recommends \
       "$DIST/libcefnest0_${VERSION}-${RELEASE}_amd64.deb" \
       "$DIST/cefnest-helper_${VERSION}-${RELEASE}_amd64.deb" \
       "$DIST/cefnest-demo_${VERSION}-${RELEASE}_amd64.deb" \
       >"$DIST/smoke-install.log" 2>&1; then
    ok "apt resolved and installed libcefnest0, cefnest-helper, cefnest-demo"
  else
    bad "the install failed — a Depends string is wrong or unsatisfiable"
    tail -20 "$DIST/smoke-install.log" | sed 's/^/         /'
  fi

  echo "== smoke: the installed loader is satisfied"
  # ldd on the *installed* paths is the point: the helper loads libcef.so from
  # $ORIGIN, and a runtime file that came along under the wrong name shows up
  # here as a "not found" rather than as a window that never paints.
  LIBSO=$(ls /usr/lib/*/libcefnest.so.0 2>/dev/null | head -1)
  [ -n "$LIBSO" ] || { bad "no installed libcefnest.so.0 to run ldd on"; }
  if [ -n "$LIBSO" ] && ldd "$LIBSO" 2>&1 | grep -q "not found"; then
    bad "ldd $LIBSO has unresolved libraries"
    ldd "$LIBSO" 2>&1 | grep "not found" | sed 's/^/         /'
  elif [ -n "$LIBSO" ]; then
    ok "ldd $LIBSO is satisfied"
  fi
  if ldd "$HELPER_DIR/cefnest-helper" 2>&1 | grep -q "not found"; then
    bad "ldd $HELPER_DIR/cefnest-helper has unresolved libraries"
    ldd "$HELPER_DIR/cefnest-helper" 2>&1 | grep "not found" | sed 's/^/         /'
  else
    ok "ldd $HELPER_DIR/cefnest-helper is satisfied"
  fi

  echo "== smoke: the packaged sandbox default"
  # Started the way a user starts it, with no display: whatever it says, it says
  # before it needs one. "No usable sandbox!" is the failure this looks for —
  # docs/internals/sandbox-policy.md §4 measured that exact transcript.
  UNSHARE=unknown
  if unshare -Urm true >/dev/null 2>&1; then
    UNSHARE=yes
  else
    UNSHARE=no
  fi
  note "unprivileged user namespaces in this container: $UNSHARE"
  HELPER_OUT=$(timeout 120 "$HELPER_DIR/cefnest-helper" --ozone-platform=wayland \
               "about:blank" 2>&1 || true)
  printf '%s\n' "$HELPER_OUT" | grep -E "helper: sandbox=|No usable sandbox|FATAL|Unable to" |
    head -6 | sed 's/^/         /'
  if printf '%s\n' "$HELPER_OUT" | grep -q "helper: sandbox=on"; then
    ok "the package starts with the sandbox on (CHROMIUM_HELPER_NO_SANDBOX is the only off switch)"
  else
    bad "the helper did not report sandbox=on; the packaged default changed"
  fi
  if printf '%s\n' "$HELPER_OUT" | grep -q "No usable sandbox"; then
    bad "the packaged helper cannot sandbox on this host — the docs in README/packaging.md are the user's only way out"
  else
    ok "no sandbox FATAL on this host"
  fi
  if printf '%s\n' "$HELPER_OUT" | grep -q "helper: sandbox=OFF"; then
    bad "CHROMIUM_HELPER_NO_SANDBOX is set in this environment; the run above proves nothing"
  fi
fi

echo
if [ "$fails" = 0 ]; then
  echo "smoke: all checks passed (CEF $CEF_VERSION / Chromium $CHROMIUM_VERSION)"
  exit 0
fi
echo "smoke: $fails check(s) failed"
exit 1
