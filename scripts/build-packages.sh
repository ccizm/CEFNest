#!/bin/sh
# Build installable Debian and RPM packages from the meson install tree.
#
# Since 154.0.34-7 the release has two packages, not four: the GTK 4 component
# library and the helper it starts are one deployment unit, so they travel
# together in `cefnest`. The helper's CEF runtime ships stripped — about
# 350 MB installed, 126 MB packed; the unpacked distribution is ~1.5 GB —
# and a library package that hard-depends on it would make every library
# install download it anyway; splitting the two only hid the cost.
#
# `cefnest-dev` carries the headers, the unversioned .so link and cefnest.pc.
# The demo and shm-client are development/test tools, not part of the package:
# they stay in cefnest-build.tar.gz and are not packaged.
#
# The version follows the CEF release the helper is built against: the package
# version is CEF's X.Y.Z, and the full CEF/Chromium strings go into the
# cefnest description. meson's shared-library version only accepts X.Y.Z, which
# is exactly the part CEF publishes before the first '+'.
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
DEB_MAIN_DIR="$ROOT/deb-cefnest"
DEB_DEV_DIR="$ROOT/deb-cefnest-dev"

# rpmbuild is asked for here, not at the RPM heading: this script configures and
# installs two full trees and copies a gigabyte and a half of CEF runtime
# before it gets there, and finding the tool missing after all of that throws
# the whole run away.
for tool in meson ninja dpkg-deb rpmbuild strip readelf nm; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "build-packages: $tool not found" >&2
    exit 1
  }
done

# Trim the CEF runtime's debug sections before anything installs from the
# source tree: the official minimal distribution ships unstripped (DWARF is
# embedded in the .so by official builds), which is 1.1 GB of disk nobody at
# runtime ever maps. Every meson tree below installs this directory through
# install_subdir at install time, so one strip here covers the deb, the rpm
# and any local packaging run. It has to happen before the cp -al
# hard-linking below: from that point the package trees share inodes with
# this directory, and stripping "one side" later would rewrite both packages.
# An unstripped copy always comes back with scripts/fetch-cef.sh (SHA-pinned)
# when a crash needs symbols.
REL="$ROOT/helper/build/Release"
if [ -d "$REL" ]; then
  for f in libcef.so libvk_swiftshader.so libvulkan.so.1 cefnest-helper; do
    if [ -f "$REL/$f" ]; then
      strip --strip-unneeded "$REL/$f"
      # A broken strip (wrong-arch binutils, truncated file) would otherwise
      # ship a library that dlopens into nothing. Dynamic symbols must
      # survive: cef_* is what the helper links against.
      n=$(readelf -S "$REL/$f" | grep -c '\.debug_' || true)
      [ "$n" -eq 0 ] || {
        echo "build-packages: $REL/$f still has $n debug section(s) after strip" >&2
        exit 1
      }
      case "$f" in
        libcef.so)
          n=$(nm -D --defined-only "$REL/$f" | grep -c ' cef_' || true)
          [ "$n" -gt 200 ] || {
            echo "build-packages: $REL/$f kept only $n cef_* exports after strip" >&2
            exit 1
          }
          ;;
      esac
    fi
  done
fi

rm -rf "$DIST" "$BUILD_DEB" "$BUILD_RPM" "$PKG_DEB" "$PKG_RPM" "$RPMBUILD" \
       "$DEB_MAIN_DIR" "$DEB_DEV_DIR"
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
  -Dinstall-demo=false \
  -Dinstall-helper=true \
  --buildtype=release
ninja -C "$BUILD_DEB"
DESTDIR="$PKG_DEB" ninja -C "$BUILD_DEB" install

# The install tree now holds exactly the component: the shared library, the
# helper with its CEF runtime, and the development files. No demo and no
# shm-client — those are built by ci-build.sh for the build tarball only.
mkdir -p "$DEB_MAIN_DIR/DEBIAN" "$DEB_MAIN_DIR/usr/lib/$MULTIARCH" \
         "$DEB_MAIN_DIR/usr/libexec/cefnest"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH"/libcefnest.so.* \
      "$DEB_MAIN_DIR/usr/lib/$MULTIARCH/"
printf 'libcefnest 0 cefnest (>= %s-%s)\n' "$VERSION" "$RELEASE" \
  > "$DEB_MAIN_DIR/DEBIAN/shlibs"

# The helper tree is the bulk of the package. Hard-link it into the package directory instead of
# copying it: dpkg-deb only reads these files, and the RPM tree below gets the
# same treatment.
cp -al "$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH" \
       "$DEB_MAIN_DIR/usr/libexec/cefnest/"

mkdir -p "$DEB_DEV_DIR/DEBIAN" "$DEB_DEV_DIR/usr/include" \
         "$DEB_DEV_DIR/usr/lib/$MULTIARCH"
cp -a "$PKG_DEB/usr/include/cefnest" "$DEB_DEV_DIR/usr/include/"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH/libcefnest.so" \
      "$DEB_DEV_DIR/usr/lib/$MULTIARCH/"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH/pkgconfig" \
      "$DEB_DEV_DIR/usr/lib/$MULTIARCH/"

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

Package: cefnest
Architecture: any
Description: CEFNest GTK 4 component library
EOF
HELPER_DEPS=$(cd "$SHLIBDEPS_DIR" &&
  dpkg-shlibdeps -O \
    -e"$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH/libcef.so" \
    -e"$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH/cefnest-helper" 2>/dev/null |
  sed -n 's/^shlibs:Depends=//p') || HELPER_DEPS=""
rm -rf "$SHLIBDEPS_DIR"
if [ -z "$HELPER_DEPS" ]; then
  HELPER_DEPS="libasound2t64, libatk-bridge2.0-0t64, libatk1.0-0t64, libatspi2.0-0t64, libc6 (>= 2.39), libcairo2, libcups2t64, libdbus-1-3, libexpat1, libgbm1, libgcc-s1, libglib2.0-0t64, libgraphene-1.0-0, libnspr4, libnss3, libpango-1.0-0, libstdc++6 (>= 13), libudev1, libx11-6, libxcb1, libxcomposite1, libxdamage1, libxext6, libxfixes3, libxkbcommon0, libxrandr2"
fi
# dpkg-shlibdeps above reads libcef and the helper, not libcefnest. What the
# library itself adds on top of those is GTK (pinned to the version the tree
# needs), GtkSourceView, graphene, the Wayland server library and json-glib;
# without graphene the package still installs by accident, because GTK happens
# to pull it in.
LIB_ONLY_DEPS="libgtk-4-1 (>= 4.14), libgtksourceview-5-0, libgraphene-1.0-0, libwayland-server0, libjson-glib-1.0-0 | libjson-glib-1.0-0t64"
MAIN_DEPS="$HELPER_DEPS, $LIB_ONLY_DEPS"

# CEF's own license text travels with the package that carries CEF's binaries,
# and is recorded in the same copyright file. meson installs CEF's runtime files
# only, so this file comes straight out of the distribution fetch-cef.sh unpacked.
CEF_LICENSE="$ROOT/third_party/cef/dist/LICENSE.txt"
[ -f "$CEF_LICENSE" ] || {
  echo "build-packages: no $CEF_LICENSE to record in cefnest" >&2
  echo "  (run ./scripts/fetch-cef.sh before packaging)" >&2
  exit 1
}

# One writer for both packages so the control fields cannot drift.
deb_control() {
  _dc_dir=$1
  _dc_package=$2
  _dc_section=$3
  _dc_depends=$4
  _dc_summary=$5
  _dc_detail=$6
  _dc_extra=${7:-}

  # DEP-5 machine-readable copyright, in both packages: a .deb without it says
  # nothing about what it carries, and the main package carries someone else's
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
    if [ -n "$_dc_extra" ]; then
      printf '%s\n' "$_dc_extra"
    fi
    printf 'Section: %s\n' "$_dc_section"
    printf 'Priority: optional\n'
    printf 'Homepage: https://cnb.cool/yanzhiming/CEFNest\n'
    printf 'Description: %s\n' "$_dc_summary"
    # A control field cannot contain an empty line: dpkg ends the value there
    # and refuses the package ("empty line in the field Description"). The
    # detail text uses blank lines for paragraphs, so turn each into Debian's
    # continuation marker " ." before indenting the whole block.
    printf '%s\n' "$_dc_detail" | sed -e 's/^$/./' -e 's/^/ /'
  } > "$_dc_dir/DEBIAN/control"
}

MIGRATE_MAIN="Replaces: libcefnest0 (<< $VERSION-$RELEASE), cefnest-helper (<< $VERSION-$RELEASE), cefnest-demo (<< $VERSION-$RELEASE)
Breaks: libcefnest0 (<< $VERSION-$RELEASE), cefnest-helper (<< $VERSION-$RELEASE), cefnest-demo (<< $VERSION-$RELEASE)
Provides: libcefnest0 (= $VERSION-$RELEASE)"

deb_control "$DEB_MAIN_DIR" cefnest libs "$MAIN_DEPS" \
  "CEFNest GTK 4 component library" \
  "The nested compositor, the GTK 4 widget and the control channel that let a
GTK application composite a page rendered by Chromium. The component installs
system-wide as libcefnest.so.0, so a host links it like any other GTK 4
library; cefnest-dev adds the headers and the pkg-config file. This package
also carries cefnest-helper and the CEF $CEF_VERSION runtime the helper loads
from its own directory. Built for Chromium $CHROMIUM_VERSION; about
350 MB installed — the CEF runtime ships stripped of debug sections.

Renderer sandboxing is ON by default, and chrome-sandbox is installed 0755:
this package installs no setuid root helper. Chromium's sandbox needs
unprivileged user namespaces, so on a host that restricts them — Ubuntu 24.04
and later, where apparmor_restrict_unprivileged_userns defaults to 1 — the
helper stops at start with 'No usable sandbox!' rather than falling back.
Either give cefnest-helper its own AppArmor profile, or set
CHROMIUM_HELPER_NO_SANDBOX=1 to say plainly that this deployment runs page
code unsandboxed.

The demo and shm-client are development/test tools and are not part of this
package; they travel in cefnest-build.tar.gz." \
  "$MIGRATE_MAIN"

# The main package carries CEF, so its copyright must record CEF's license
# next to the LGPL text the helper above wrote.
cp -p "$CEF_LICENSE" "$DEB_MAIN_DIR/usr/share/doc/cefnest/LICENSE.cef.txt"
{
  printf '\nFiles: usr/libexec/cefnest/*\n'
  printf 'Copyright: 2008-2020 Marshall A. Greenblatt; portions 2006-2009 Google Inc.\n'
  printf 'License: BSD-3-clause\n'
  sed 's/^/ /' "$CEF_LICENSE"
  printf '\nFiles: usr/share/doc/cefnest/LICENSE.cef.txt\n'
  printf 'Copyright: 2008-2020 Marshall A. Greenblatt; portions 2006-2009 Google Inc.\n'
  printf 'License: BSD-3-clause\n'
} >> "$DEB_MAIN_DIR/usr/share/doc/cefnest/copyright"

deb_control "$DEB_DEV_DIR" cefnest-dev libdevel \
  "cefnest (= $VERSION-$RELEASE), libgtk-4-dev (>= 4.14), libgtksourceview-5-dev, libwayland-dev, libxkbcommon-dev, libjson-glib-dev, libgbm-dev, libdrm-dev" \
  "CEFNest development files" \
  "Headers, the unversioned library link and the pkg-config file needed to
build a host application against CEFNest." \
  "Replaces: libcefnest-dev (<< $VERSION-$RELEASE)
Breaks: libcefnest-dev (<< $VERSION-$RELEASE)
Provides: libcefnest-dev (= $VERSION-$RELEASE)"

DEB_MAIN="cefnest_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"
DEB_DEV="cefnest-dev_${VERSION}-${RELEASE}_${DEB_ARCH}.deb"

dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_MAIN_DIR" "$DIST/$DEB_MAIN"
dpkg-deb --build -Zxz -z1 --root-owner-group "$DEB_DEV_DIR" "$DIST/$DEB_DEV"

# ---------------------------------------------------------------------------
# RPM
# ---------------------------------------------------------------------------

echo "== rpm: configure and install"
meson setup "$BUILD_RPM" \
  -Dprefix=/usr \
  -Dlibdir=/usr/lib64 \
  -Dcefnest-version="$VERSION" \
  -Dinstall-demo=false \
  -Dinstall-helper=true \
  --buildtype=release
ninja -C "$BUILD_RPM"
DESTDIR="$PKG_RPM" ninja -C "$BUILD_RPM" install

# meson install copied the whole helper tree twice. Replace the RPM copy with hard
# links to the Debian tree; the files are identical and both trees live in the
# same workspace.
rm -rf "$PKG_RPM/usr/libexec/cefnest/$HELPER_ARCH"
cp -al "$PKG_DEB/usr/libexec/cefnest/$HELPER_ARCH" \
       "$PKG_RPM/usr/libexec/cefnest/"

# RPM's own license convention. The project text is LGPL-2.1-only; the helper files are
# BSD-3-Clause, and both have to be named by a %files section or rpmbuild
# fails — which is the check that they really travel with the package.
mkdir -p "$PKG_RPM/usr/share/licenses/cefnest"
cp -p "$ROOT/LICENSE" "$PKG_RPM/usr/share/licenses/cefnest/LICENSE"
cp -p "$CEF_LICENSE" "$PKG_RPM/usr/share/licenses/cefnest/LICENSE.cef.txt"

cat > "$RPMBUILD/SPECS/cefnest.spec" <<EOF
Name: cefnest
Version: $VERSION
Release: $RELEASE
Summary: CEFNest GTK 4 component library, with CEF helper and runtime
License: LGPL-2.1-only and BSD-3-Clause
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
Requires: gtksourceview5
Requires: libwayland-server
Requires: libxkbcommon
Requires: json-glib
Requires: mesa-libgbm
Requires: libdrm

# 154.0.34-6 shipped the library, helper and demo as three packages. The helper
# and demo now live in this one; saying so lets dnf replace them instead of
# refusing on file conflicts.
Obsoletes: cefnest-helper < %{version}-%{release}
Obsoletes: cefnest-demo < %{version}-%{release}
Provides: cefnest-helper = %{version}-%{release}
Provides: cefnest-demo = %{version}-%{release}

%description
The nested compositor, the GTK 4 widget and the control channel that let a
GTK application composite a page rendered by Chromium. The component installs
system-wide as libcefnest.so.0; the devel package adds the headers and the
pkg-config file. This package also carries cefnest-helper and the CEF
$CEF_VERSION runtime the helper loads from its own directory. Built for
Chromium $CHROMIUM_VERSION; about 350 MB installed — the CEF runtime
ships stripped of debug sections.

Renderer sandboxing is ON by default and chrome-sandbox is installed 0755:
this package installs no setuid root helper. Chromium's sandbox needs
unprivileged user namespaces, so on a host that restricts them the helper
stops at start with 'No usable sandbox!' rather than falling back. Either
give cefnest-helper its own AppArmor profile, or set
CHROMIUM_HELPER_NO_SANDBOX=1 to say plainly that this deployment runs page
code unsandboxed.

The demo and shm-client are development/test tools and are not part of this
package; they travel in cefnest-build.tar.gz.

%package devel
Summary: Development files for CEFNest
License: LGPL-2.1-only
Requires: cefnest = %{version}-%{release}
Requires: gtk4-devel >= 4.14
Requires: gtksourceview5-devel
Requires: wayland-devel
Requires: libxkbcommon-devel
Requires: json-glib-devel
Requires: mesa-libgbm-devel
Requires: libdrm-devel

%description devel
Headers, the unversioned library link and the pkg-config file needed to build
a host application against CEFNest.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -al %{_cefnest_pkgroot}/usr %{buildroot}/

%files
/usr/lib64/libcefnest.so.*
/usr/libexec/cefnest/$HELPER_ARCH
/usr/share/licenses/cefnest/LICENSE
/usr/share/licenses/cefnest/LICENSE.cef.txt

%files devel
/usr/include/cefnest/*.h
/usr/lib64/libcefnest.so
/usr/lib64/pkgconfig/cefnest.pc

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig
EOF

rpmbuild -bb --noclean \
  --define "_topdir $RPMBUILD" \
  --define "_cefnest_pkgroot $PKG_RPM" \
  "$RPMBUILD/SPECS/cefnest.spec"

RPM_MAIN="cefnest-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"
RPM_DEV="cefnest-devel-${VERSION}-${RELEASE}.${RPM_ARCH}.rpm"

cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_MAIN" "$DIST/$RPM_MAIN"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_DEV"  "$DIST/$RPM_DEV"

# ---------------------------------------------------------------------------
# Checksums and a summary
# ---------------------------------------------------------------------------

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
ls -l
