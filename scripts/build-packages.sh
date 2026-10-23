#!/bin/sh
# Build installable Debian and RPM packages from the meson install tree.
#
# The helper is part of the packages now: cefnest-helper is a CMake/CEF
# executable, and the CEF runtime it loads through $ORIGIN has to travel with
# it — about 1.5 GB. The helper goes into its own package so a host that
# already has a helper (or only drives shm-client) does not have to download it.
#
# The version follows the CEF release the helper is built against: the package
# version is CEF's X.Y.Z, and the full CEF/Chromium strings go into the
# cefnest-helper description. meson's shared-library version only accepts
# X.Y.Z, which is exactly the part CEF publishes before the first '+'.
#
# The build tree used by the CNB attachment keeps its own prefix. This script
# configures two extra trees, one for Debian's multiarch libdir and one for the
# /usr/lib64 layout RPM distributions use, so neither package has to relocate
# files after `meson install`.
set -eu
umask 022

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CEF_VERSION=$(cat "$ROOT/third_party/cef/dist/.cef-version" 2>/dev/null || true)
CHROMIUM_VERSION=$(cat "$ROOT/third_party/cef/dist/.chromium-version" 2>/dev/null || true)
VERSION=$(cat "$ROOT/third_party/cef/dist/.cef-package-version" 2>/dev/null || true)
if [ -z "$VERSION" ] && [ -n "$CEF_VERSION" ]; then
  VERSION=$(printf '%s\n' "$CEF_VERSION" |
            sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
fi
[ -n "$VERSION" ] || {
  echo "build-packages: no CEF version under third_party/cef/dist;" >&2
  echo "  run scripts/fetch-cef.sh first" >&2
  exit 1
}
[ -n "$CEF_VERSION" ] || CEF_VERSION=unknown
[ -n "$CHROMIUM_VERSION" ] || CHROMIUM_VERSION=unknown

# The package revision: same CEF, rebuilt sources. Package managers treat
# 154.0.34-1 and 154.0.34-2 as different versions, so a re-release must bump
# this — the CNB pipeline exports CEFNEST_PACKAGE_RELEASE for tag builds
# (.cnb.yml). Local builds default to 1.
RELEASE=${CEFNEST_PACKAGE_RELEASE:-1}

HELPER_BUILD="$ROOT/helper/build/Release"
[ -x "$HELPER_BUILD/cefnest-helper" ] || {
  echo "build-packages: $HELPER_BUILD/cefnest-helper is missing" >&2
  echo "  run scripts/fetch-cef.sh, then:" >&2
  echo "  cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build helper/build -j\"\$(nproc)\"" >&2
  exit 1
}
HELPER_ARCH=$(uname -m)

DEB_ARCH=$(dpkg --print-architecture)
RPM_ARCH=$(uname -m)
MULTIARCH=$(gcc -print-multiarch 2>/dev/null || true)
[ -n "$MULTIARCH" ] || {
  echo "build-packages: could not determine the compiler's multiarch tuple" >&2
  exit 1
}

DIST="$ROOT/dist"
BUILD_DEB="$ROOT/build-pkg-deb"
BUILD_RPM="$ROOT/build-pkg-rpm"
PKG_DEB="$ROOT/pkg-deb"
PKG_RPM="$ROOT/pkg-rpm"
RPMBUILD="$ROOT/rpmbuild"

# rpmbuild is asked for here, not at the RPM heading: this script configures and
# installs two full trees and copies a gigabyte and a half of CEF runtime
# before it gets there, and finding the tool missing after all of that throws
# the whole run away.
for tool in meson ninja dpkg-deb rpmbuild; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "build-packages: $tool not found" >&2
    exit 1
  }
done

rm -rf "$DIST" "$BUILD_DEB" "$BUILD_RPM" "$PKG_DEB" "$PKG_RPM" "$RPMBUILD"
mkdir -p "$DIST"
mkdir -p "$RPMBUILD/BUILD" "$RPMBUILD/BUILDROOT" "$RPMBUILD/RPMS" \
         "$RPMBUILD/SOURCES" "$RPMBUILD/SPECS" "$RPMBUILD/SRPMS"

# ---------------------------------------------------------------------------
# Debian
# ---------------------------------------------------------------------------

echo "== deb: configure and install"
meson setup "$BUILD_DEB" \
  -Dprefix=/usr \
  -Dcefnest-version="$VERSION" \
  -Dinstall-demo=true \
  -Dinstall-helper=true \
  --buildtype=release
ninja -C "$BUILD_DEB"
DESTDIR="$PKG_DEB" ninja -C "$BUILD_DEB" install

# Split the install tree into the four packages Debian expects: the runtime
# library, the development files, the demo with its fallback client, and the
# helper with the CEF runtime that has to sit next to it.
DEB_LIB_DIR="$ROOT/deb-libcefnest0"
DEB_DEV_DIR="$ROOT/deb-libcefnest-dev"
DEB_DEMO_DIR="$ROOT/deb-cefnest-demo"
DEB_HELPER_DIR="$ROOT/deb-cefnest-helper"
rm -rf "$DEB_LIB_DIR" "$DEB_DEV_DIR" "$DEB_DEMO_DIR" "$DEB_HELPER_DIR"

mkdir -p "$DEB_LIB_DIR/DEBIAN" "$DEB_LIB_DIR/usr/lib/$MULTIARCH"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH"/libcefnest.so.* \
      "$DEB_LIB_DIR/usr/lib/$MULTIARCH/"
printf 'libcefnest 0 libcefnest0 (>= %s-1)\n' "$VERSION" \
  > "$DEB_LIB_DIR/DEBIAN/shlibs"

mkdir -p "$DEB_DEV_DIR/DEBIAN" "$DEB_DEV_DIR/usr/include" \
         "$DEB_DEV_DIR/usr/lib/$MULTIARCH"
cp -a "$PKG_DEB/usr/include/cefnest" "$DEB_DEV_DIR/usr/include/"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH/libcefnest.so" \
      "$DEB_DEV_DIR/usr/lib/$MULTIARCH/"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH/pkgconfig" \
      "$DEB_DEV_DIR/usr/lib/$MULTIARCH/"

mkdir -p "$DEB_DEMO_DIR/DEBIAN" "$DEB_DEMO_DIR/usr/bin" \
         "$DEB_DEMO_DIR/usr/libexec/cefnest"
cp -a "$PKG_DEB/usr/bin/cefnest-demo" "$DEB_DEMO_DIR/usr/bin/"
cp -a "$PKG_DEB/usr/libexec/cefnest/shm-client" \
      "$DEB_DEMO_DIR/usr/libexec/cefnest/"

# The helper tree is 1.5 GB. Hard-link it into the package directory instead of
# copying it: dpkg-deb only reads these files, and the RPM tree below gets the
# same treatment.
mkdir -p "$DEB_HELPER_DIR/DEBIAN" "$DEB_HELPER_DIR/usr/libexec/cefnest"
cp -al "$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH" \
       "$DEB_HELPER_DIR/usr/libexec/cefnest/"

# dpkg-shlibdeps needs a control file to know which binary package it is
# working on. A throwaway one keeps the real package writer below unchanged.
SHLIBDEPS_DIR=$(mktemp -d)
mkdir -p "$SHLIBDEPS_DIR/debian"
cat > "$SHLIBDEPS_DIR/debian/control" <<EOF
Source: cefnest
Section: misc
Priority: optional
Maintainer: siem <yan@zhiming.xyz>
Standards-Version: 4.6.2

Package: cefnest-helper
Architecture: any
Description: CEF runtime and cefnest-helper
EOF
HELPER_DEPS=$(cd "$SHLIBDEPS_DIR" &&
  dpkg-shlibdeps -O \
    -e"$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH/libcef.so" \
    -e"$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH/cefnest-helper" 2>/dev/null |
  sed -n 's/^shlibs:Depends=//p') || HELPER_DEPS=""
rm -rf "$SHLIBDEPS_DIR"
if [ -z "$HELPER_DEPS" ]; then
  HELPER_DEPS="libasound2t64, libatk-bridge2.0-0t64, libatk1.0-0t64, libatspi2.0-0t64, libc6 (>= 2.39), libcairo2, libcups2t64, libdbus-1-3, libexpat1, libgbm1, libgcc-s1, libglib2.0-0t64, libnspr4, libnss3, libpango-1.0-0, libstdc++6 (>= 13), libudev1, libx11-6, libxcb1, libxcomposite1, libxdamage1, libxext6, libxfixes3, libxkbcommon0, libxrandr2"
fi
# The dependency graph is strictly layered, never circular: the helper (the CEF
# runtime) is the bottom layer and installs first, the library depends on it,
# and dev/demo depend on the library.

# One writer for all four so the control fields cannot drift.
deb_control() {
  _dc_dir=$1
  _dc_package=$2
  _dc_section=$3
  _dc_depends=$4
  _dc_summary=$5
  _dc_detail=$6
  _dc_recommends=${7:-}

  # DEP-5 machine-readable copyright, in all four packages: a .deb without it
  # says nothing about what it carries, and the helper carries someone else's
  # code, which is exactly the case that needs saying.
  #
  # It goes in the data tree, not in DEBIAN/. A binary package's DEBIAN/
  # members are dpkg's own control files; dpkg never installs a copyright from
  # there, so lintian still reports E: no-copyright-file. Policy §12.5 puts the
  # file at /usr/share/doc/<package>/copyright, which is where every installed
  # package on this host keeps it.
  mkdir -p "$_dc_dir/usr/share/doc/$_dc_package"
  {
    printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
    printf 'Upstream-Name: cefnest\n'
    printf 'Upstream-Contact: siem <yan@zhiming.xyz>\n'
    printf 'Source: https://cnb.cool/yanzhiming/CEFNest\n'
    printf '\nFiles: *\nCopyright: 2026 Siem\nLicense: LGPL-2.1\n'
    sed 's/^/ /' "$ROOT/LICENSE"
  } > "$_dc_dir/usr/share/doc/$_dc_package/copyright"

  # Measured after the copyright exists, so Installed-Size counts it.
  _dc_size=$(du -sk "$_dc_dir/usr" | cut -f1)

  {
    printf 'Package: %s\n' "$_dc_package"
    printf 'Version: %s-%s\n' "$VERSION" "$RELEASE"
    printf 'Architecture: %s\n' "$DEB_ARCH"
    printf 'Maintainer: siem <yan@zhiming.xyz>\n'
    printf 'Installed-Size: %s\n' "$_dc_size"
    if [ -n "$_dc_depends" ]; then
      printf 'Depends: %s\n' "$_dc_depends"
    fi
    if [ -n "$_dc_recommends" ]; then
      printf 'Recommends: %s\n' "$_dc_recommends"
    fi
    printf 'Section: %s\n' "$_dc_section"
    printf 'Priority: optional\n'
    printf 'Homepage: https://cnb.cool/yanzhiming/CEFNest\n'
    printf 'Description: %s\n' "$_dc_summary"
    printf '%s\n' "$_dc_detail" | sed 's/^/ /'
  } > "$_dc_dir/DEBIAN/control"
}

deb_control "$DEB_LIB_DIR" libcefnest0 libs \
  "libc6 (>= 2.39), libgtk-4-1 (>= 4.14), libwayland-server0, libxkbcommon0, libjson-glib-1.0-0 | libjson-glib-1.0-0t64, libgbm1, libdrm2, cefnest-helper (= $VERSION-$RELEASE)" \
  "CEFNest shared library" \
  "The compositor, the GTK 4 widget and the control channel that let a GTK
application composite a page rendered by Chromium." \
  ""

deb_control "$DEB_DEV_DIR" libcefnest-dev libdevel \
  "libcefnest0 (= $VERSION-$RELEASE), libgtk-4-dev (>= 4.14), libwayland-dev, libxkbcommon-dev, libjson-glib-dev, libgbm-dev, libdrm-dev" \
  "CEFNest development files" \
  "Headers, the unversioned library link and the pkg-config file needed to
build a host application against CEFNest." \
  ""

deb_control "$DEB_DEMO_DIR" cefnest-demo utils \
  "libc6 (>= 2.39), libcefnest0 (= $VERSION-$RELEASE), cefnest-helper (= $VERSION-$RELEASE), libgtk-4-1 (>= 4.14), libwayland-client0, libxkbcommon0, libjson-glib-1.0-0 | libjson-glib-1.0-0t64, libgbm1, libdrm2" \
  "CEFNest demo and headless test client" \
  "The demo window and shm-client, the fallback client it drives when no CEF
helper is installed." \
  ""

# CEF's own license text travels with the package that carries CEF's binaries,
# and is recorded in the same copyright file. meson installs the runtime only,
# so this file comes straight out of the distribution fetch-cef.sh unpacked.
CEF_LICENSE="$ROOT/third_party/cef/dist/LICENSE.txt"
[ -f "$CEF_LICENSE" ] || {
  echo "build-packages: no $CEF_LICENSE to record in cefnest-helper" >&2
  echo "  (run ./scripts/fetch-cef.sh before packaging)" >&2
  exit 1
}
mkdir -p "$DEB_HELPER_DIR/usr/share/doc/cefnest-helper"
cp -p "$CEF_LICENSE" "$DEB_HELPER_DIR/usr/share/doc/cefnest-helper/LICENSE.cef.txt"

deb_control "$DEB_HELPER_DIR" cefnest-helper misc "$HELPER_DEPS" \
  "CEF runtime and cefnest-helper" \
  "The cefnest-helper executable and the CEF $CEF_VERSION runtime it loads
from its own directory. Built for Chromium $CHROMIUM_VERSION; about 1.5 GB
installed.
Renderer sandboxing is ON by default, and chrome-sandbox is installed 0755:
this package installs no setuid root helper. Chromium's sandbox needs
unprivileged user namespaces, so on a host that restricts them — Ubuntu 24.04
and later, where apparmor_restrict_unprivileged_userns defaults to 1 — the
helper stops at start with 'No usable sandbox!' rather than falling back.
Either give cefnest-helper its own AppArmor profile, or set
CHROMIUM_HELPER_NO_SANDBOX=1 to say plainly that this deployment runs page
code unsandboxed. docs/internals/sandbox-policy.md §6 in the source tree is
the measurement behind that choice." \
  ""

{
  printf '\nFiles: usr/libexec/cefnest/*\n'
  printf 'Copyright: 2008-2020 Marshall A. Greenblatt; portions 2006-2009 Google Inc.\n'
  printf 'License: BSD-3-clause\n'
  sed 's/^/ /' "$CEF_LICENSE"
  printf '\nFiles: usr/share/doc/cefnest-helper/LICENSE.cef.txt\n'
  printf 'Copyright: 2008-2020 Marshall A. Greenblatt; portions 2006-2009 Google Inc.\n'
  printf 'License: BSD-3-clause\n'
} >> "$DEB_HELPER_DIR/usr/share/doc/cefnest-helper/copyright"

DEB_LIB="libcefnest0_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"
DEB_DEV="libcefnest-dev_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"
DEB_DEMO="cefnest-demo_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"
DEB_HELPER="cefnest-helper_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"

dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_LIB_DIR" "$DIST/$DEB_LIB"
dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_DEV_DIR" "$DIST/$DEB_DEV"
dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_DEMO_DIR" "$DIST/$DEB_DEMO"
dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_HELPER_DIR" "$DIST/$DEB_HELPER"

# ---------------------------------------------------------------------------
# RPM
# ---------------------------------------------------------------------------

echo "== rpm: configure and install"
meson setup "$BUILD_RPM" \
  -Dprefix=/usr \
  -Dlibdir=/usr/lib64 \
  -Dcefnest-version="$VERSION" \
  -Dinstall-demo=true \
  -Dinstall-helper=true \
  --buildtype=release
ninja -C "$BUILD_RPM"
DESTDIR="$PKG_RPM" ninja -C "$BUILD_RPM" install

# meson install copied the 1.5 GB helper twice. Replace the RPM copy with hard
# links to the Debian tree; the files are identical and both trees live in the
# same workspace.
rm -rf "$PKG_RPM/usr/libexec/cefnest/$HELPER_ARCH"
cp -al "$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH" \
       "$PKG_RPM/usr/libexec/cefnest/"

# Same license text the deb carries. rpmbuild fails the build if a file in the
# tree is not named in a %files section, so the entry below is the check.
mkdir -p "$PKG_RPM/usr/share/doc/cefnest-helper"
cp -p "$CEF_LICENSE" "$PKG_RPM/usr/share/doc/cefnest-helper/LICENSE.cef.txt"

cat > "$RPMBUILD/SPECS/cefnest.spec" <<EOF
Name: cefnest
Version: $VERSION
Release: $RELEASE
Summary: GTK 4 widget that shows a page rendered by Chromium
License: LGPL-2.1-only
URL: https://cnb.cool/yanzhiming/CEFNest
Packager: siem <yan@zhiming.xyz>
BuildArch: $RPM_ARCH

# The files are already built and installed by meson; rpmbuild only packs them.
# Disabling the install post-processing also keeps the host's brp scripts out of
# a cross-distribution package build.
%global __os_install_post %{nil}
%global debug_package %{nil}
%global _build_id_links none
%define _binary_payload w2.xzdio

Requires: glibc >= 2.39
Requires: gtk4 >= 4.14
Requires: libwayland-server
Requires: libxkbcommon
Requires: json-glib
Requires: mesa-libgbm
Requires: libdrm
Requires: cefnest-helper = %{version}-%{release}

%description
The compositor, the GTK 4 widget and the control channel that let a GTK
application composite a page rendered by Chromium.

%package devel
Summary: Development files for CEFNest
Requires: cefnest = %{version}-%{release}
Requires: gtk4-devel >= 4.14
Requires: wayland-devel
Requires: libxkbcommon-devel
Requires: json-glib-devel
Requires: mesa-libgbm-devel
Requires: libdrm-devel

%description devel
Headers, the unversioned library link and the pkg-config file needed to build
a host application against CEFNest.

%package helper
Summary: CEF runtime and cefnest-helper
License: LGPL-2.1-only and BSD-3-Clause
Requires: glibc >= 2.39

%description helper
The cefnest-helper executable and the CEF $CEF_VERSION runtime it loads from
its own directory. Built for Chromium $CHROMIUM_VERSION; about 1.5 GB installed.

Renderer sandboxing is ON by default and chrome-sandbox is installed 0755: this
package installs no setuid root helper. Chromium's sandbox needs unprivileged
user namespaces, so on a host that restricts them the helper stops at start with
'No usable sandbox!' rather than falling back. Either give cefnest-helper its own
AppArmor profile, or set CHROMIUM_HELPER_NO_SANDBOX=1 to say plainly that this
deployment runs page code unsandboxed.

%package demo
Summary: CEFNest demo and headless test client
Requires: cefnest = %{version}-%{release}
Requires: cefnest-helper = %{version}-%{release}
Requires: glibc >= 2.39
Requires: gtk4 >= 4.14
Requires: libwayland-client
Requires: libxkbcommon
Requires: json-glib
Requires: mesa-libgbm
Requires: libdrm

%description demo
The demo window and shm-client, the fallback client it drives when no CEF
helper is installed.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -al %{_cefnest_pkgroot}/usr %{buildroot}/

%files
/usr/lib64/libcefnest.so.*

%files devel
/usr/lib64/libcefnest.so
/usr/include/cefnest/*.h
/usr/lib64/pkgconfig/cefnest.pc

%files helper
/usr/libexec/cefnest/$HELPER_ARCH
/usr/share/doc/cefnest-helper/LICENSE.cef.txt

%files demo
/usr/bin/cefnest-demo
/usr/libexec/cefnest/shm-client

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig
EOF

rpmbuild -bb --noclean \
  --define "_topdir $RPMBUILD" \
  --define "_cefnest_pkgroot $PKG_RPM" \
  "$RPMBUILD/SPECS/cefnest.spec"

RPM_LIB="cefnest-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"
RPM_DEV="cefnest-devel-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"
RPM_HELPER="cefnest-helper-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"
RPM_DEMO="cefnest-demo-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"

cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_LIB"    "$DIST/$RPM_LIB"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_DEV"    "$DIST/$RPM_DEV"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_HELPER" "$DIST/$RPM_HELPER"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_DEMO"   "$DIST/$RPM_DEMO"

# ---------------------------------------------------------------------------
# Archive, checksums and a summary
# ---------------------------------------------------------------------------

TARBALL="cefnest-packages-${VERSION}-${RELEASE}.tar.gz"

# The helper packages are hundreds of megabytes; putting them inside this
# tarball would only compress them a second time. They are separate attachments.
tar -czf "$DIST/$TARBALL" -C "$DIST" \
  "$DEB_LIB" "$DEB_DEV" "$DEB_DEMO" \
  "$RPM_LIB" "$RPM_DEV" "$RPM_DEMO"

# One more file belongs in this manifest but was not made here: the release
# pipeline's build tarball. CEFNEST_EXTRA_ARTIFACTS names it relative to the
# repo root and it moves into dist/, because a checksum file described as
# covering "every artifact" that misses one of the uploaded names is worse than
# no checksum file at all — and a missing name here fails the stage loudly.
for extra in ${CEFNEST_EXTRA_ARTIFACTS:-}; do
  [ -f "$extra" ] || {
    echo "build-packages: CEFNEST_EXTRA_ARTIFACTS says $extra, which is not there" >&2
    exit 1
  }
  mv "$extra" "$DIST/"
done

(
  cd "$DIST"
  sha256sum ./*.deb ./*.rpm ./*.tar.gz > SHA256SUMS
)

echo "== packages in $DIST"
ls -l "$DIST"
