#!/bin/sh
# ci-smoke-packages.sh — install the packages the way a user does, then check
# what a user would find.
#
# Since 154.0.34-7 there are two packages: cefnest (the GTK 4 component library
# plus cefnest-helper and its CEF runtime) and cefnest-dev (headers, link .so and
# cefnest.pc). The demo and shm-client are development/test tools and are not
# packaged; they are checked out of cefnest-build.tar.gz instead.
#
# The tag pipeline used to build the packages and stop there: nothing proved the
# Depends strings (hand-written, in scripts/build-packages.sh) name libraries
# that exist, that the CEF runtime really travels next to the helper, or that
# the 1.5 GB of files satisfy each other's loader. All three are the kind of
# thing a user finds out by having nothing on screen.
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
# are reported side by side.
#
# Run after scripts/ci-build.sh, in the same workspace, as root:
#
#   ./scripts/ci-smoke-packages.sh
#
# The release pipeline runs scripts/ci-smoke-job.sh instead, which prepares the
# host (tzdata, then the compiler and binutils this script's own two checks need)
# and then calls this script.
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
V="$VERSION-$RELEASE"
DEB_ARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
MULTIARCH=$(gcc -print-multiarch 2>/dev/null || true)

MAIN_DEB="$DIST/cefnest_${V}_${DEB_ARCH}.deb"
DEV_DEB="$DIST/cefnest-dev_${V}_${DEB_ARCH}.deb"
MAIN_RPM="$DIST/cefnest-${V}.${ARCH}.rpm"
DEV_RPM="$DIST/cefnest-devel-${V}.${ARCH}.rpm"

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
for expected in cefnest-build.tar.gz "$MAIN_DEB" "$DEV_DEB" "$MAIN_RPM" "$DEV_RPM" SHA256SUMS; do
  case "$expected" in
    /*) [ -f "$expected" ] || bad "missing release asset: $expected" ;;
    *)  [ -f "$DIST/$expected" ] || bad "missing release asset: $DIST/$expected" ;;
  esac
done
if [ -f "$MAIN_DEB" ] && [ -f "$DEV_DEB" ] &&
   [ -f "$MAIN_RPM" ] && [ -f "$DEV_RPM" ]; then
  ok "the two packages exist in both formats"
fi

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

# The old four-package layout and the bundle should not come back by accident.
if ls "$DIST"/libcefnest0_* "$DIST"/libcefnest-dev_* \
      "$DIST"/cefnest-helper_* "$DIST"/cefnest-demo_* \
      "$DIST"/cefnest-packages-* >/dev/null 2>&1; then
  bad "dist/ contains a package from the retired -6 layout"
else
  ok "no retired package names or package bundle in dist/"
fi

echo "== smoke: package contents"
# dpkg -c needs no root and no install: it reads the archive. Checking the names
# inside the .deb is what catches "the helper is there but its runtime is not"
# before anything tries to load it.
MAIN_LIST=$(dpkg -c "$MAIN_DEB" 2>/dev/null || true)
for needed in "$HELPER_DIR/cefnest-helper" "$HELPER_DIR/libcef.so" \
              "$HELPER_DIR/icudtl.dat" "$HELPER_DIR/resources.pak" \
              "$HELPER_DIR/v8_context_snapshot.bin" "$HELPER_DIR/chrome-sandbox" \
              "$HELPER_DIR/locales"; do
  case "$MAIN_LIST" in
    *"$needed"*) ok "cefnest carries $needed" ;;
    *) bad "cefnest does not carry $needed" ;;
  esac
done

case "$MAIN_LIST" in
  *"libcefnest.so.$VERSION"*) ok "cefnest carries the versioned library object" ;;
  *) bad "cefnest has no libcefnest.so.$VERSION — check -Dcefnest-version" ;;
esac
case "$MAIN_LIST" in
  *cefnest-demo*) bad "cefnest carries cefnest-demo; the demo belongs to the build tree" ;;
  *) ok "cefnest carries no demo" ;;
esac
case "$MAIN_LIST" in
  *shm-client*) bad "cefnest carries shm-client; it is a test client" ;;
  *) ok "cefnest carries no shm-client" ;;
esac

DEV_LIST=$(dpkg -c "$DEV_DEB" 2>/dev/null || true)
for needed in "usr/include/cefnest/chromium-session.h" \
              "usr/include/cefnest/chromium-view.h" \
              "usr/include/cefnest/nested.h" \
              "pkgconfig/cefnest.pc"; do
  case "$DEV_LIST" in
    *"$needed"*) ok "cefnest-dev carries $needed" ;;
    *) bad "cefnest-dev does not carry $needed" ;;
  esac
done
case "$DEV_LIST" in
  *libcef.so*) bad "cefnest-dev carries the CEF runtime; that belongs in cefnest" ;;
  *) ok "cefnest-dev carries no CEF runtime" ;;
esac

# chrome-sandbox is deliberately 0755: this project ships no setuid helper and
# no AppArmor profile it has not measured.
# The mode is asserted so a future packaging change cannot quietly turn it into
# a privilege-escalation surface nobody reviewed.
SBX_MODE=$(printf '%s\n' "$MAIN_LIST" |
  awk -v p="$HELPER_DIR/chrome-sandbox" '$0 ~ p { print substr($1, 1, 10); exit }')
if [ "$SBX_MODE" = "-rwxr-xr-x" ]; then
  ok "chrome-sandbox is 0755 (not setuid, as decided)"
else
  bad "chrome-sandbox mode reads as '${SBX_MODE:-unknown}', expected -rwxr-xr-x"
fi

# Policy §12.5: a binary package's copyright file lives in the data tree, at
# /usr/share/doc/<package>/copyright. This is checked where it is read from —
# reading the member out of the archive costs one pass over the 1.5 GB and runs
# without root, so the reduced CI_SMOKE_SKIP_INSTALL=1 pass still checks it.
MAIN_DEP5=$(dpkg-deb --fsys-tarfile "$MAIN_DEB" 2>/dev/null |
  tar -xO ./usr/share/doc/cefnest/copyright 2>/dev/null || true)
case "$MAIN_DEP5" in
  *copyright-format/1.0*License:\ LGPL-2.1*)
    ok "cefnest ships /usr/share/doc/cefnest/copyright" ;;
  *) bad "cefnest has no usable /usr/share/doc/cefnest/copyright" ;;
esac
case "$MAIN_DEP5" in
  *"Files: usr/libexec/cefnest/"*BSD-3-clause*)
    ok "cefnest records the CEF runtime as BSD-3-clause" ;;
  *) bad "cefnest does not record a license for the CEF runtime it carries" ;;
esac
case "$MAIN_LIST" in
  *"usr/share/doc/cefnest/LICENSE.cef.txt"*) ok "cefnest carries CEF's license text" ;;
  *) bad "cefnest does not carry CEF's license text" ;;
esac

DEV_DEP5=$(dpkg-deb --fsys-tarfile "$DEV_DEB" 2>/dev/null |
  tar -xO ./usr/share/doc/cefnest-dev/copyright 2>/dev/null || true)
case "$DEV_DEP5" in
  *copyright-format/1.0*License:\ LGPL-2.1*)
    ok "cefnest-dev ships /usr/share/doc/cefnest-dev/copyright" ;;
  *) bad "cefnest-dev has no usable /usr/share/doc/cefnest-dev/copyright" ;;
esac

if ls "$DIST"/*.rpm >/dev/null 2>&1; then
  echo "== smoke: rpm contents (listed, not installed — this is a Debian host)"
  command -v rpm >/dev/null 2>&1 || { note "no rpm tool: skipping the rpm listing"; }
  if command -v rpm >/dev/null 2>&1; then
    for rpm_name in "$MAIN_RPM" "$DEV_RPM"; do
      [ -f "$rpm_name" ] || { bad "no $rpm_name"; continue; }
      if rpm -qp --requires "$rpm_name" >/dev/null 2>&1; then
        ok "rpm -qp readable: $(basename "$rpm_name")"
      else
        bad "rpm -qp failed on $rpm_name"
      fi
    done
    case "$(rpm -qlp "$MAIN_RPM" 2>/dev/null || true)" in
      *"$HELPER_DIR/libcef.so"*) ok "the cefnest rpm carries the CEF runtime too" ;;
      *) bad "the cefnest rpm does not list $HELPER_DIR/libcef.so" ;;
    esac
    case "$(rpm -qlp "$MAIN_RPM" 2>/dev/null || true)" in
      *cefnest-demo*) bad "the cefnest rpm carries the demo" ;;
      *) ok "the cefnest rpm carries no demo" ;;
    esac
    case "$(rpm -qlp "$DEV_RPM" 2>/dev/null || true)" in
      *cefnest.pc*) ok "the devel rpm carries cefnest.pc" ;;
      *) bad "the devel rpm does not list cefnest.pc" ;;
    esac
    # Reported, not asserted: the spec's own Requires for the main package is a
    # short list, so whether a host is told about libnss3/libgbm and the rest
    # comes from rpmbuild's own soname discovery — which %global
    # __os_install_post %{nil} does *not* cover (that macro stops the brp
    # post-processing, not find-requires). Nobody in this repo has an rpm host
    # to have measured it on, so the line below is the measurement. Update it
    # from what a real run prints.
    note "cefnest rpm --requires: $(rpm -qp --requires "$MAIN_RPM" 2>/dev/null |
      tr '\n' ' ')"
  fi
fi

if [ "$AS_ROOT" = 1 ]; then
  echo "== smoke: apt installs these Depends"
  # This is the check the hand-written Depends strings have never had: apt
  # resolves each name against the archive and refuses the install if one is
  # wrong, invented, or older than the library needs.
  #
  # tzdata goes first, through the same script the build stage uses. It is not
  # decoration: the -8 run's only failure had tzdata at the head of its
  # `dpkg: error processing package … (--configure)` chain, and python3.12, the
  # GTK -dev chain and cefnest-dev itself unconfigured behind it — none of which
  # names our Depends. Configured on its own beforehand, tzdata is out of the way
  # and a failure here is about our packages again. The mechanism is a postinst
  # renaming over a bind-mounted /etc/localtime.
  export DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC
  apt-get update -qq
  ./scripts/ci-prepare-tzdata.sh
  if apt-get install -y --no-install-recommends "$MAIN_DEB" "$DEV_DEB" \
       >"$DIST/smoke-install.log" 2>&1; then
    ok "apt resolved and installed cefnest and cefnest-dev"
  else
    # A non-zero apt has several causes and "a Depends string is wrong" is only
    # one of them, and the tail apt prints — the list of packages left
    # unconfigured — names none: it does not say which package broke or why. The
    # 2026-10-23 -7 run failed on exactly that, with 17 distro postinsts in the
    # error list, our own two packages installed and ldd-clean on the next lines,
    # and nothing readable about the cause. So print the head of the error chain
    # with the reason above it, the state our packages reached, and the disk the
    # container had.
    bad "the install failed — the lines after this one name the cause"
    first=$(grep -n -m1 -E '^dpkg: error processing package' \
              "$DIST/smoke-install.log" 2>/dev/null | cut -d: -f1 || true)
    if [ -n "$first" ]; then
      # dpkg prints the summary line "error processing package X" *after* the
      # postinst output that says why, so the reason is a few lines above it.
      from=$((first - 8))
      if [ "$from" -lt 1 ]; then from=1; fi
      sed -n "${from},$((first + 1))p" "$DIST/smoke-install.log" | sed 's/^/         /'
    fi
    grep -m3 -E '^E:|[Nn]o space left on device' \
      "$DIST/smoke-install.log" | sed 's/^/         /' || true
    state=$(dpkg -l cefnest cefnest-dev 2>/dev/null |
              awk '$1 ~ /^[a-z][a-zA-Z]$/ { printf "%s %s %s|", $1, $2, $3 }')
    printf '         our packages: %s\n' "${state:-dpkg has no record of them}"
    printf '         disk: %s\n' \
      "$(df -h / /usr /var 2>/dev/null | sed -n '1!p' | sort -u | tr '\n' '|')"
    tail -20 "$DIST/smoke-install.log" | sed 's/^/         /'
  fi

  echo "== smoke: the installed loader is satisfied"
  # ldd on the *installed* paths is the point: the helper loads libcef.so from
  # $ORIGIN, and a CEF runtime file that came along under the wrong name shows up
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
  if [ -e /usr/bin/cefnest-demo ]; then
    bad "the cefnest install put a cefnest-demo binary in /usr/bin"
  else
    ok "no demo binary was installed"
  fi

  echo "== smoke: pkg-config finds the installed development files"
  PKG_CONFIG_PATH=$(ls -d /usr/lib/*/pkgconfig 2>/dev/null | head -1 || true)
  export PKG_CONFIG_PATH
  if pkg-config --exists cefnest; then
    ok "pkg-config --exists cefnest"
  else
    bad "pkg-config cannot find cefnest.pc"
  fi
  PROBE_DIR=$(mktemp -d)
  cat > "$PROBE_DIR/probe.c" <<'PROBE'
#include <cefnest/chromium-session.h>
#include <stdio.h>

int
main (void)
{
  ChromiumSession *session = chromium_session_new (NULL);
  const char *path = chromium_session_get_helper_path (session);

  printf ("%s\n", path);
  g_object_unref (session);
  return 0;
}
PROBE
  # A missing compiler is a fact about the image, not about the package: the
  # bare ubuntu:24.04 the pipeline runs on has no cc, and the -7 run read that as
  # "the pkg-config compile failed". Skipped out loud instead, and .cnb.yml
  # installs gcc + binutils in front of this script so the check really runs.
  if ! command -v cc >/dev/null 2>&1; then
    note "no cc in this image: the pkg-config compile check did not run"
  elif cc "$PROBE_DIR/probe.c" -o "$PROBE_DIR/probe" \
       $(pkg-config --cflags --libs cefnest) >"$PROBE_DIR/cc.log" 2>&1; then
    ok "a host compiles with pkg-config --cflags --libs cefnest"
    PROBE_OUT=$("$PROBE_DIR/probe" 2>&1 || true)
    if [ "$PROBE_OUT" = "$HELPER_DIR/cefnest-helper" ]; then
      ok "the installed library names the packaged helper path"
    else
      bad "helper path reads as '${PROBE_OUT:-<empty>}', expected $HELPER_DIR/cefnest-helper"
    fi
  else
    bad "the pkg-config compile failed"
    tail -20 "$PROBE_DIR/cc.log" | sed 's/^/         /'
  fi
  rm -rf "$PROBE_DIR"

  echo "== smoke: the build-tree demo names the installed helper"
  DEMO_TREE=$(mktemp -d)
  if tar -xzf "$DIST/cefnest-build.tar.gz" -C "$DEMO_TREE"; then
    if [ -x "$DEMO_TREE/src/cefnest-demo" ]; then
      ok "cefnest-build.tar.gz carries src/cefnest-demo"
      # The compiled-in helper path lives in libcefnest, not in the demo: the
      # demo only links against it. Asking the demo would fail even on a
      # correct tree, so read the library that sits next to it.
      TREE_LIB="$DEMO_TREE/src/libcefnest.so.$VERSION"
      if ! command -v strings >/dev/null 2>&1; then
        note "no strings in this image: the build-tree helper-path check did not run"
      elif [ -f "$TREE_LIB" ] &&
         strings "$TREE_LIB" | grep -qF "$HELPER_DIR/cefnest-helper"; then
        ok "the build-tree library was compiled with the package helper path"
      else
        bad "the build-tree library does not name $HELPER_DIR/cefnest-helper; check -Dprefix=/usr"
      fi
      if ldd "$DEMO_TREE/src/cefnest-demo" 2>&1 | grep -q "not found"; then
        bad "ldd on the build-tree demo has unresolved libraries"
        ldd "$DEMO_TREE/src/cefnest-demo" 2>&1 | grep "not found" | sed 's/^/         /'
      else
        ok "ldd on the build-tree demo is satisfied"
      fi
    else
      bad "cefnest-build.tar.gz has no executable src/cefnest-demo"
    fi
    if [ -x "$DEMO_TREE/test/shm-client" ]; then
      ok "cefnest-build.tar.gz carries test/shm-client"
    else
      bad "cefnest-build.tar.gz has no executable test/shm-client"
    fi
  else
    bad "could not unpack cefnest-build.tar.gz"
  fi
  rm -rf "$DEMO_TREE"

  echo "== smoke: the packaged sandbox default"
  # Started the way a user starts it, with no display: whatever it says, it says
  # before it needs one. "No usable sandbox!" is the failure this looks for.
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
