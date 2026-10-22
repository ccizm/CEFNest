#!/bin/sh
# Build installable Debian and RPM packages from the meson install tree.
#
# The library, the widget headers, the pkg-config file and the demo are what a
# host needs; the helper is not here, because it is a CMake/CEF build and the
# CEF runtime is architecture-specific and about 1.5 GB. A package that carries
# a demo without a helper is still useful: the demo falls back to shm-client,
# which is installed next to it under libexec.
#
# The build tree used by the CNB attachment keeps its own prefix. This script
# configures two extra trees, one for Debian's multiarch libdir and one for the
# /usr/lib64 layout RPM distributions use, so neither package has to relocate
# files after `meson install`.
set -eu
umask 022

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

VERSION=$(sed -n "s/^  version: '\([^']*\)',/\1/p" meson.build | head -n 1)
[ -n "$VERSION" ] || {
  echo "build-packages: could not read the project version from meson.build" >&2
  exit 1
}

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

for tool in meson ninja dpkg-deb; do
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
  -Dinstall-demo=true \
  -Dinstall-helper=false \
  --buildtype=release
ninja -C "$BUILD_DEB"
DESTDIR="$PKG_DEB" ninja -C "$BUILD_DEB" install

# Split the install tree into the three packages Debian expects: the runtime
# library, the development files, and the demo with its fallback client.
DEB_LIB_DIR="$ROOT/deb-libcefnest0"
DEB_DEV_DIR="$ROOT/deb-libcefnest-dev"
DEB_DEMO_DIR="$ROOT/deb-cefnest-demo"
rm -rf "$DEB_LIB_DIR" "$DEB_DEV_DIR" "$DEB_DEMO_DIR"

mkdir -p "$DEB_LIB_DIR/DEBIAN" "$DEB_LIB_DIR/usr/lib/$MULTIARCH"
cp -a "$PKG_DEB/usr/lib/$MULTIARCH"/libcefnest.so.0* \
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

# One description writer for all three so the control fields cannot drift.
deb_control() {
  _dc_dir=$1
  _dc_package=$2
  _dc_section=$3
  _dc_depends=$4
  _dc_summary=$5
  _dc_detail=$6
  _dc_size=$(du -sk "$_dc_dir/usr" | cut -f1)

  {
    printf 'Package: %s\n' "$_dc_package"
    printf 'Version: %s-1\n' "$VERSION"
    printf 'Architecture: %s\n' "$DEB_ARCH"
    printf 'Maintainer: CEFNest packagers <cefnest@example.invalid>\n'
    printf 'Installed-Size: %s\n' "$_dc_size"
    if [ -n "$_dc_depends" ]; then
      printf 'Depends: %s\n' "$_dc_depends"
    fi
    printf 'Section: %s\n' "$_dc_section"
    printf 'Priority: optional\n'
    printf 'Homepage: https://cnb.cool/yanzhiming/CEFNest\n'
    printf 'Description: %s\n' "$_dc_summary"
    printf '%s\n' "$_dc_detail" | sed 's/^/ /'
  } > "$_dc_dir/DEBIAN/control"
}

deb_control "$DEB_LIB_DIR" libcefnest0 libs \
  "libc6 (>= 2.39), libgtk-4-1 (>= 4.14), libwayland-server0, libxkbcommon0, libjson-glib-1.0-0 | libjson-glib-1.0-0t64, libgbm1, libdrm2" \
  "CEFNest shared library" \
  "The compositor, the GTK 4 widget and the control channel that let a GTK
application composite a page rendered by Chromium."

deb_control "$DEB_DEV_DIR" libcefnest-dev libdevel \
  "libcefnest0 (= $VERSION-1), libgtk-4-dev (>= 4.14), libwayland-dev, libxkbcommon-dev, libjson-glib-dev, libgbm-dev, libdrm-dev" \
  "CEFNest development files" \
  "Headers, the unversioned library link and the pkg-config file needed to
build a host application against CEFNest."

deb_control "$DEB_DEMO_DIR" cefnest-demo utils \
  "libc6 (>= 2.39), libcefnest0 (= $VERSION-1), libgtk-4-1 (>= 4.14), libwayland-client0, libxkbcommon0, libjson-glib-1.0-0 | libjson-glib-1.0-0t64, libgbm1, libdrm2" \
  "CEFNest demo and headless test client" \
  "The demo window and shm-client, the fallback client it drives when no CEF
helper is installed."

DEB_LIB="libcefnest0_${VERSION}-1_${DEB_ARCH}.deb"
DEB_DEV="libcefnest-dev_${VERSION}-1_${DEB_ARCH}.deb"
DEB_DEMO="cefnest-demo_${VERSION}-1_${DEB_ARCH}.deb"

dpkg-deb --build --root-owner-group "$DEB_LIB_DIR" "$DIST/$DEB_LIB"
dpkg-deb --build --root-owner-group "$DEB_DEV_DIR" "$DIST/$DEB_DEV"
dpkg-deb --build --root-owner-group "$DEB_DEMO_DIR" "$DIST/$DEB_DEMO"

# Fixed names for the CNB attachment list: the version is in the package
# metadata, and the attachment list cannot expand a shell variable.
cp "$DIST/$DEB_LIB"  "$DIST/libcefnest0.deb"
cp "$DIST/$DEB_DEV"  "$DIST/libcefnest-dev.deb"
cp "$DIST/$DEB_DEMO" "$DIST/cefnest-demo.deb"

# ---------------------------------------------------------------------------
# RPM
# ---------------------------------------------------------------------------

command -v rpmbuild >/dev/null 2>&1 || {
  echo "build-packages: rpmbuild not found; install the rpm package" >&2
  exit 1
}

echo "== rpm: configure and install"
meson setup "$BUILD_RPM" \
  -Dprefix=/usr \
  -Dlibdir=/usr/lib64 \
  -Dinstall-demo=true \
  -Dinstall-helper=false \
  --buildtype=release
ninja -C "$BUILD_RPM"
DESTDIR="$PKG_RPM" ninja -C "$BUILD_RPM" install

cat > "$RPMBUILD/SPECS/cefnest.spec" <<EOF
Name: cefnest
Version: $VERSION
Release: 1
Summary: GTK 4 widget that shows a page rendered by Chromium
License: LGPL-2.1-only
URL: https://cnb.cool/yanzhiming/CEFNest
BuildArch: $RPM_ARCH
AutoReqProv: no

# The files are already built and installed by meson; rpmbuild only packs them.
# Disabling the install post-processing also keeps the host's brp scripts out of
# a cross-distribution package build.
%global __os_install_post %{nil}
%global debug_package %{nil}
%global _build_id_links none

Requires: glibc >= 2.39
Requires: gtk4 >= 4.14
Requires: libwayland-server
Requires: libxkbcommon
Requires: json-glib
Requires: mesa-libgbm
Requires: libdrm
Provides: libcefnest.so.0()(64bit)

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

%package demo
Summary: CEFNest demo and headless test client
Requires: cefnest = %{version}-%{release}
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
cp -a %{_cefnest_pkgroot}/usr %{buildroot}/

%files
/usr/lib64/libcefnest.so.0*

%files devel
/usr/lib64/libcefnest.so
/usr/include/cefnest/*.h
/usr/lib64/pkgconfig/cefnest.pc

%files demo
/usr/bin/cefnest-demo
/usr/libexec/cefnest/shm-client

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig
EOF

rpmbuild -bb \
  --define "_topdir $RPMBUILD" \
  --define "_cefnest_pkgroot $PKG_RPM" \
  "$RPMBUILD/SPECS/cefnest.spec"

RPM_LIB="cefnest-${VERSION}-1.${RPM_ARCH}.rpm"
RPM_DEV="cefnest-devel-${VERSION}-1.${RPM_ARCH}.rpm"
RPM_DEMO="cefnest-demo-${VERSION}-1.${RPM_ARCH}.rpm"

cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_LIB"  "$DIST/$RPM_LIB"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_DEV"  "$DIST/$RPM_DEV"
cp "$RPMBUILD/RPMS/$RPM_ARCH/$RPM_DEMO" "$DIST/$RPM_DEMO"

cp "$DIST/$RPM_LIB"  "$DIST/cefnest.rpm"
cp "$DIST/$RPM_DEV"  "$DIST/cefnest-devel.rpm"
cp "$DIST/$RPM_DEMO" "$DIST/cefnest-demo.rpm"

# ---------------------------------------------------------------------------
# Archive, checksums and a summary
# ---------------------------------------------------------------------------

tar -czf "$DIST/cefnest-packages.tar.gz" -C "$DIST" \
  "$DEB_LIB" "$DEB_DEV" "$DEB_DEMO" \
  "$RPM_LIB" "$RPM_DEV" "$RPM_DEMO"

(
  cd "$DIST"
  sha256sum ./*.deb ./*.rpm ./cefnest-packages.tar.gz > SHA256SUMS
)

echo "== packages in $DIST"
ls -l "$DIST"
