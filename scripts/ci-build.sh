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

# Read the version before configuring anything: meson's shared-library version,
# the file name of libcefnest.so.X.Y.Z, the pkg-config file, the package names
# and the Release title all have to come from this one string. build-packages.sh
# derives the same value the same way; CEFNEST_PACKAGE_RELEASE is shared with it
# for the same reason.
CEF_VERSION=$(cat third_party/cef/dist/.cef-version)
CHROMIUM_VERSION=$(cat third_party/cef/dist/.chromium-version)
PACKAGE_VERSION=$(cat third_party/cef/dist/.cef-package-version)
RELEASE=${CEFNEST_PACKAGE_RELEASE:-1}
V="$PACKAGE_VERSION-$RELEASE"

rm -rf helper/build
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j"$(nproc 2>/dev/null || echo 2)"

rm -rf builddir
# -Dcefnest-version is what meson.options exists for. Left out, this tree builds
# libcefnest.so.0.0.1 — meson's own default — while the deb beside it in the
# same Release carries libcefnest.so.$PACKAGE_VERSION, and the two version
# strings a reader is told to compare turn out to describe different files.
#
# -Dprefix=/usr is what makes the build tree's demo usable against the packages:
# the helper path is compiled into the demo from the prefix, and the package
# installs it at /usr/libexec/cefnest/<arch>/. With the default /usr/local the
# demo would look under /usr/local and not find the helper it was shipped beside.
meson setup builddir -Dprefix=/usr -Dcefnest-version="$PACKAGE_VERSION"
ninja -C builddir

# Keep src/, examples/ and test/ as directories. The demo looks for its library
# at $ORIGIN/ and every example and test client at $ORIGIN/../src; flattened
# into one directory those rpaths point at nothing, and "unpack and run" would
# quietly mean "unpack, set LD_LIBRARY_PATH, and run".
rm -rf artifact
mkdir -p artifact/src artifact/examples artifact/test
cp -a builddir/src/libcefnest.so \
  "builddir/src/libcefnest.so.0" \
  "builddir/src/libcefnest.so.$PACKAGE_VERSION" artifact/src/
cp -a builddir/src/cefnest-demo artifact/src/
# ninja already failed this stage if any example failed to build, so the only
# thing left to get wrong is the copy — and a swallowed cp error is how two
# examples came to be missing from a tarball described as holding all of them.
for exe in builddir/examples/cefnest-*; do
  [ -f "$exe" ] && [ -x "$exe" ] || continue
  cp -a "$exe" artifact/examples/
done
[ -n "$(ls artifact/examples)" ] || {
  echo "cnb: no examples in the build tree — nothing to put in the tarball" >&2
  exit 1
}
cp -a builddir/test/shm-client artifact/test/
cp -a builddir/test/seat-selftest artifact/test/

EXAMPLE_COUNT=$(ls artifact/examples | wc -l)
tar -czf cefnest-build.tar.gz -C artifact .

# The manifest belongs to build-packages.sh, which owns dist/ — so the tarball is
# handed over by name and build-packages.sh moves it in and hashes it. That is
# the one place SHA256SUMS is written, and after this line the file lives in
# dist/, which is also where .cnb.yml uploads it from.
CEFNEST_EXTRA_ARTIFACTS=cefnest-build.tar.gz ./scripts/build-packages.sh

cat > RELEASE-NOTES.md <<EOF
# CEFNest $V

CEF $CEF_VERSION (Chromium $CHROMIUM_VERSION)

## 中文

本版只改发布形态，组件行为与 154.0.34-6 相同：

- 安装包从四个收敛为两个：\`cefnest\` 是 GTK 4 组件库本身，连同它启动的
  \`cefnest-helper\`（含完整 CEF 运行时）一起交付；\`cefnest-dev\` 只放头文件、
  链接用 \`.so\` 与 \`cefnest.pc\`。组件库以系统库 \`libcefnest.so.0\` 安装，
  原本就精确依赖同版本 helper，拆开只是把 1.5 GB 的下载藏到依赖里；现在它
  直接成为主包的一部分。
- \`cefnest-demo\` 与 \`shm-client\` 不再单独发包。它们是开发/测试工具，留在
  \`cefnest-build.tar.gz\` 的 \`src/\` 与 \`test/\` 里；CI 仍用构建树跑它们。
- 构建树改为按 \`-Dprefix=/usr\` 编译，解包后的 demo 会直接找到安装包放在
  \`/usr/libexec/cefnest/<arch>/\` 的 helper，不再需要设置 \`CHROMIUM_HELPER\`。
- 旧包名可平滑升级：deb 用 \`Replaces\`/\`Breaks\`，rpm 用 \`Obsoletes\`，从 -6
  升级不需要手工卸载 \`libcefnest0\`、\`cefnest-helper\` 或 \`cefnest-demo\`。
- 取消 \`cefnest-packages-*.tar.gz\`：两个包已经包含全量，合集只会把同一份
  deb 与 rpm 再打包一遍。

### 附件说明

| 文件 | 说明 |
| --- | --- |
| cefnest-build.tar.gz | 构建树：\`src/\`（\`libcefnest.so\`、\`.so.0\`、\`.so.$PACKAGE_VERSION\`、\`cefnest-demo\`）、\`examples/\`（$EXAMPLE_COUNT 个示例）、\`test/\`（\`shm-client\`、\`seat-selftest\`）。解包后保持这三层目录，二进制会用自身的 \`\$ORIGIN\` rpath 找到同批的库。**不含 helper、不含 CEF 运行时**：装 \`cefnest\` 后，\`src/cefnest-demo\` 会直接找到安装位置里的 helper |
| cefnest_${V}_amd64.deb | GTK 4 组件库：\`libcefnest.so.0\` + \`cefnest-helper\` + 完整 CEF 运行时（安装后约 1.5 GB） |
| cefnest-dev_${V}_amd64.deb | 开发文件：三个头文件、链接用 \`.so\`、\`cefnest.pc\` |
| cefnest-$V.x86_64.rpm | GTK 4 组件库（rpm） |
| cefnest-devel-$V.x86_64.rpm | 开发文件（rpm） |
| SHA256SUMS | dist/ 下全部上传物的 SHA-256 清单，含 cefnest-build.tar.gz |

---

## English

This release changes the packaging only; the component's behaviour is unchanged
from 154.0.34-6.

- The release now has two packages instead of four: \`cefnest\` is the GTK 4
  component library itself, shipped with the \`cefnest-helper\` it starts and
  the complete CEF runtime; \`cefnest-dev\` carries the headers, the unversioned
  \`.so\` and \`cefnest.pc\`. The library installs system-wide as
  \`libcefnest.so.0\` and already hard-depended on the exact helper version, so
  the split only hid a 1.5 GB download behind a dependency; it is now part of
  the main package.
- \`cefnest-demo\` and \`shm-client\` are no longer packaged. They are
  development/test tools and travel in \`cefnest-build.tar.gz\` under \`src/\`
  and \`test/\`; CI still runs them from there.
- The build tree is configured with \`-Dprefix=/usr\`, so the unpacked demo
  finds the installed helper at \`/usr/libexec/cefnest/<arch>/\` without
  \`CHROMIUM_HELPER\`.
- Old package names upgrade cleanly: deb uses \`Replaces\`/\`Breaks\`, rpm uses
  \`Obsoletes\`; upgrading from -6 does not require removing \`libcefnest0\`,
  \`cefnest-helper\` or \`cefnest-demo\` by hand.
- \`cefnest-packages-*.tar.gz\` is gone: the two packages already carry
  everything, and the bundle would only repack the same deb and rpm.

### Assets

| File | Description |
| --- | --- |
| cefnest-build.tar.gz | Build tree: \`src/\` (\`libcefnest.so\`, \`.so.0\`, \`.so.$PACKAGE_VERSION\`, \`cefnest-demo\`), \`examples/\` ($EXAMPLE_COUNT examples), \`test/\` (\`shm-client\`, \`seat-selftest\`). Keep the three directories when unpacking — the binaries find this same library through their own \`\$ORIGIN\` rpath. **No helper, no CEF runtime**: after installing \`cefnest\`, \`src/cefnest-demo\` finds the installed helper directly |
| cefnest_${V}_amd64.deb | GTK 4 component library: \`libcefnest.so.0\` + \`cefnest-helper\` + complete CEF runtime (~1.5 GB installed) |
| cefnest-dev_${V}_amd64.deb | Development files: headers, link \`.so\`, \`cefnest.pc\` |
| cefnest-$V.x86_64.rpm | GTK 4 component library (rpm) |
| cefnest-devel-$V.x86_64.rpm | Development files (rpm) |
| SHA256SUMS | SHA-256 manifest for everything in dist/, cefnest-build.tar.gz included |

Install \`cefnest\` to run a host application or the build-tree demo; add
\`cefnest-dev\` only when compiling against the library. The demo is not part of
either package.
EOF

echo "== ci build done"
ls -lh dist/cefnest-build.tar.gz dist/SHA256SUMS RELEASE-NOTES.md
