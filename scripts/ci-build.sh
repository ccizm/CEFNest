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
meson setup builddir -Dcefnest-version="$PACKAGE_VERSION"
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

本版变更：

- 新增自定义 User-Agent：宿主把 \`CHROMIUM_USER_AGENT\` 放进环境即可，HTTP 头与页面 JS 同时生效（examples/cefnest-useragent 演示运行中切换）。
- 修复窗口焦点与选中：点击页面/窗口即切换 browser-selected，右键菜单落在正确的窗口；切标签不再藏掉独立窗口。
- 修复并发宿主的 CEF 缓存冲突：缓存目录按宿主 socket 派生，多宿主并发互不影响。
- 新增 11 个完整示例（examples/）：单页浏览器、多标签、多窗口、JS 控制台、查找条、下载、自绘菜单、权限、错误恢复、帧率观测、UA 切换。
- helper 现在收 SIGTERM/SIGHUP/SIGINT：\`systemctl stop\` 与桌面注销会走完 CefShutdown()，不再在 profile 里留下 SingletonLock（docs/features/session-lifecycle.md §7）。
- 打包产物统一携带版本与修订号（$V）：构建树里的 \`libcefnest.so.$PACKAGE_VERSION\` 与包名里的版本是同一个串。

### 附件说明

| 文件 | 说明 |
| --- | --- |
| cefnest-build.tar.gz | 构建树：\`src/\`（\`libcefnest.so\`、\`libcefnest.so.0\`、\`libcefnest.so.$PACKAGE_VERSION\`、\`cefnest-demo\`）、\`examples/\`（$EXAMPLE_COUNT 个示例）、\`test/\`（\`shm-client\`、\`seat-selftest\`）。解包后保持这三层目录，二进制会用自身的 \`\$ORIGIN\` rpath 找到同批的库。**不含 helper、不含 CEF 运行时**：要渲染页面得装下面的 cefnest-helper 包，或用 \`CHROMIUM_HELPER\` 指到一个已构建的 helper |
| libcefnest0_${V}_amd64.deb | 运行库：合成器、GTK 4 部件与控制通道 |
| libcefnest-dev_${V}_amd64.deb | 开发文件：头文件、链接用 .so、cefnest.pc |
| cefnest-helper_${V}_amd64.deb | helper 与完整 CEF 运行时（约 1.5 GB）。它是 libcefnest0 的依赖，装库会把它一起拉进来 |
| cefnest-demo_${V}_amd64.deb | 演示浏览器与回退客户端 shm-client |
| cefnest-$V.x86_64.rpm | 运行库（rpm） |
| cefnest-devel-$V.x86_64.rpm | 开发文件（rpm） |
| cefnest-helper-$V.x86_64.rpm | helper 与 CEF 运行时（rpm） |
| cefnest-demo-$V.x86_64.rpm | demo 与 shm-client（rpm） |
| cefnest-packages-$V.tar.gz | 除 helper 外 6 个包的归档 |
| SHA256SUMS | dist/ 下全部上传物的 SHA-256 清单，含 cefnest-build.tar.gz |

---

## English

Highlights of this release:

- Custom User-Agent: put \`CHROMIUM_USER_AGENT\` in the host environment and the
  helper bakes it into CefSettings.user_agent — HTTP headers and page JS alike
  (examples/cefnest-useragent switches it live).
- Window focus and selection: clicks move browser-selected, so context menus
  land in the right window; tab switches no longer blank independent windows.
- Concurrent-host CEF cache collisions are gone: the cache directory is derived
  from the host socket path.
- 11 new complete examples under examples/: browser, tabs, windows, JS console,
  find bar, downloads, custom menus, permissions, error recovery, frame
  observation, UA switching.
- The helper now answers SIGTERM/SIGHUP/SIGINT: \`systemctl stop\` and a session
  logout run CefShutdown() to the end instead of leaving a SingletonLock in the
  profile (docs/features/session-lifecycle.md §7).
- One version string everywhere ($V): the \`libcefnest.so.$PACKAGE_VERSION\` in
  the build tarball is the same version the packages carry.

### Assets

| File | Description |
| --- | --- |
| cefnest-build.tar.gz | Build tree: \`src/\` (\`libcefnest.so\`, \`.so.0\`, \`.so.$PACKAGE_VERSION\`, \`cefnest-demo\`), \`examples/\` ($EXAMPLE_COUNT examples), \`test/\` (\`shm-client\`, \`seat-selftest\`). Keep the three directories when unpacking — the binaries find this same library through their own \`\$ORIGIN\` rpath. **No helper, no CEF runtime**: render a page by installing the cefnest-helper package below, or point \`CHROMIUM_HELPER\` at a built helper |
| libcefnest0_${V}_amd64.deb | Runtime library: compositor, GTK 4 widget, control channel |
| libcefnest-dev_${V}_amd64.deb | Development files: headers, link .so, cefnest.pc |
| cefnest-helper_${V}_amd64.deb | Helper and full CEF runtime (~1.5 GB). libcefnest0 depends on it, so installing the library pulls this in |
| cefnest-demo_${V}_amd64.deb | Demo browser and the shm-client fallback |
| cefnest-$V.x86_64.rpm | Runtime library (rpm) |
| cefnest-devel-$V.x86_64.rpm | Development files (rpm) |
| cefnest-helper-$V.x86_64.rpm | Helper and CEF runtime (rpm) |
| cefnest-demo-$V.x86_64.rpm | Demo and shm-client (rpm) |
| cefnest-packages-$V.tar.gz | Archive of the six packages except the helper |
| SHA256SUMS | SHA-256 manifest for everything in dist/, cefnest-build.tar.gz included |

The helper packages carry the CEF runtime. Dependencies are layered, never
circular: install cefnest-helper first, libcefnest0 depends on it, and the
-dev and demo packages pull in the library.
EOF

echo "== ci build done"
ls -lh dist/cefnest-build.tar.gz dist/SHA256SUMS RELEASE-NOTES.md
