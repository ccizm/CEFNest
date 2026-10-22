#!/bin/sh
# Build the CNB artifacts and packages.
#
# This is the whole build stage, shared by the branch pipeline and the tag
# pipeline. It runs on Ubuntu 24.04: apt first (with tzdata handled without
# replacing a bind-mounted /etc/localtime), then the latest stable CEF, the
# helper, the meson tree, and the deb/rpm packages.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

export DEBIAN_FRONTEND=noninteractive
export TZ=Etc/UTC
SUDO=""
if [ "$(id -u)" != 0 ]; then SUDO=sudo; fi

# tzdata's postinst asks debconf for a zone even in a bare Ubuntu image.
# Preseed it as well as setting the frontend: a postinst that cannot answer
# aborts the whole apt transaction, and every package after it is left
# unpacked.
if command -v debconf-set-selections >/dev/null 2>&1; then
  printf 'tzdata tzdata/Areas select Etc\n' |
    $SUDO debconf-set-selections
  printf 'tzdata tzdata/Zones/Etc select UTC\n' |
    $SUDO debconf-set-selections
fi

# The runner bind-mounts /etc/localtime from the host. tzdata's postinst
# renames /etc/localtime.dpkg-new over it; rename(2) of a mount point is EBUSY
# and takes the whole apt transaction down. Try to unmount it first; if that
# is not permitted, unpack tzdata and skip only that rename. /etc/timezone
# still gets written and TZ is exported for every later command.
if awk '$2 == "/etc/localtime" { found = 1 } END { exit !found }' \
     /proc/mounts 2>/dev/null; then
  $SUDO umount /etc/localtime 2>/dev/null || true
fi
$SUDO apt-get update
if awk '$2 == "/etc/localtime" { found = 1 } END { exit !found }' \
     /proc/mounts 2>/dev/null; then
  TZ_TMP=$(mktemp -d)
  (cd "$TZ_TMP" && apt-get download tzdata)
  set -- "$TZ_TMP"/tzdata_*.deb
  $SUDO dpkg --unpack "$1"
  TZ_POSTINST=/var/lib/dpkg/info/tzdata.postinst
  if ! $SUDO grep -q 'if test "$(get_timezone)" != "$TIMEZONE"; then' \
       "$TZ_POSTINST"; then
    echo "cnb: unexpected tzdata postinst; refusing to guess" >&2
    exit 1
  fi
  $SUDO sed -i \
    's|if test "$(get_timezone)" != "$TIMEZONE"; then|if false; then|' \
    "$TZ_POSTINST"
  $SUDO dpkg --configure tzdata
  rm -rf "$TZ_TMP"
else
  $SUDO apt-get install -y --no-install-recommends tzdata
fi
# Repair anything the failed run left unpacked, now that tzdata can be
# configured without touching the mount.
$SUDO dpkg --configure -a
$SUDO apt-get -f install -y

$SUDO apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config git linux-libc-dev \
  libgtk-4-dev libwayland-dev libxkbcommon-dev libjson-glib-dev \
  libgbm-dev libdrm-dev wayland-protocols rpm \
  cmake curl bzip2 ca-certificates python3 \
  libasound2t64 libatk-bridge2.0-0t64 libatk1.0-0t64 \
  libatspi2.0-0t64 libcairo2 libcups2t64 libdbus-1-3 libexpat1 \
  libnspr4 libnss3 libpango-1.0-0 libudev1 libx11-6 libx11-xcb1 \
  libxcb1 libxcb-dri3-0 libxcb-render0 libxcb-shm0 libxcomposite1 \
  libxdamage1 libxext6 libxfixes3 libxrandr2 libxshmfence1 \
  libvulkan1 libwayland-client0 libwayland-cursor0 \
  libwayland-egl1 libegl1 libgles2 libgtk-3-0t64

./scripts/fetch-cef.sh
rm -rf helper/build
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j"$(nproc 2>/dev/null || echo 2)"

rm -rf builddir
meson setup builddir
ninja -C builddir

mkdir -p artifact
cp -a builddir/src/libcefnest.so \
  builddir/src/libcefnest.so.0 \
  builddir/src/libcefnest.so.0.0.1 artifact/
cp -a builddir/src/cefnest-demo artifact/
cp -a builddir/examples/cefnest-host artifact/ 2>/dev/null || true
cp -a builddir/examples/cefnest-minimal artifact/ 2>/dev/null || true
cp -a builddir/test/shm-client artifact/
cp -a builddir/test/seat-selftest artifact/
tar -czf cefnest-build.tar.gz -C artifact .

./scripts/build-packages.sh

CEF_VERSION=$(cat third_party/cef/dist/.cef-version)
CHROMIUM_VERSION=$(cat third_party/cef/dist/.chromium-version)
PACKAGE_VERSION=$(cat third_party/cef/dist/.cef-package-version)
cat > RELEASE-NOTES.md <<EOF
CEFNest packages built against CEF $CEF_VERSION (Chromium $CHROMIUM_VERSION).

Package version: $PACKAGE_VERSION-1.

Debian:
  libcefnest0, libcefnest-dev, cefnest-demo, cefnest-helper

RPM:
  cefnest, cefnest-devel, cefnest-demo, cefnest-helper

The helper packages carry the CEF runtime; cefnest-demo depends on
cefnest-helper, and libcefnest0 recommends it.
EOF

echo "== ci build done"
ls -lh cefnest-build.tar.gz RELEASE-NOTES.md
