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

- helper 现在真的收 SIGTERM/SIGHUP/SIGINT：\`systemctl stop\` 与桌面注销会走完
  \`CefShutdown()\`，不再在 profile 里留下一枚没人释放的 SingletonLock；新门禁
  \`test/helper-signal-gate.sh\` 从 \`SigCgt\`、日志、退出与锁释放四处量这条路径。
- 发版流水线多了两道当场失败的检查：推 tag 先由 \`scripts/check-release-tag.sh\`
  比版本串、修订号与 9 个静态附件名（漏改一处的结论从 90 分钟后提到几十秒），
  构建完再由 \`scripts/ci-smoke-packages.sh\` 用 apt 真装一遍、对安装路径跑 \`ldd\`、
  不带显示起一次 helper 读它的沙箱横幅。
- \`cefnest-build.tar.gz\` 现在对得上它自己的说明：\`src/\`、\`examples/\`（全部示例）、
  \`test/\` 三层保持原样，二进制用自身的 \`\$ORIGIN\` rpath 找到同批的库，库版本与包版本
  是同一个串；它**不含 helper、不含 CEF 运行时**，说明里就这么写。
- 运行前提写进文档与包描述：helper 默认开沙箱，\`chrome-sandbox\` 以 0755 打包（无
  setuid 位、无 AppArmor profile）。宿主禁掉非特权用户命名空间时——Ubuntu 24.04 默认
  如此——helper 在启动那步响亮失败，而不是静默降回无沙箱；两条出路是给
  \`cefnest-helper\` 一条允许 \`userns\` 的 AppArmor profile，或设
  \`CHROMIUM_HELPER_NO_SANDBOX=1\`。
- 许可信息补齐：四个 deb 各带 machine-readable copyright
  （\`/usr/share/doc/<包名>/copyright\`），helper 那份额外把 CEF 运行时记成
  BSD-3-clause 并附上 \`LICENSE.cef.txt\`；RPM 的 helper 子包 \`License:\` 写
  \`LGPL-2.1-only and BSD-3-Clause\`。
- \`scripts/fetch-cef.sh\` 把两个架构 \`154.0.34\` 归档的 sha256 钉进仓库：下载到的字节
  必须等于仓库批准的值才解包，不再只信构建服务自己给的摘要。
- 示例修正：zoom 按 \`100 × 1.2^level\` 显示（此前当线性百分比，放大一档显示 50%）；
  上下文菜单的 popover 挂到 view 而非窗口（\`::context-menu\` 的 x/y 是 view 内坐标）；
  下载撞名落名保住扩展名且不再泄漏；paint stack 释放；查找条只在打开时吞 Escape；
  静音图标随状态更新。
- \`src/chromium-session.h\` 的 API 文档订正：关标签同样会发 \`::browser-closed\`
  （上一版文档还在宣称它静默失败）。

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

- The helper answers SIGTERM/SIGHUP/SIGINT for real: \`systemctl stop\` and a
  session logout run \`CefShutdown()\` to the end instead of leaving a
  SingletonLock nobody will release, and \`test/helper-signal-gate.sh\` grades the
  whole path from \`SigCgt\`, the transcript, the exit and the released lock.
- Two new checks that fail on the spot. \`scripts/check-release-tag.sh\` compares
  the version, the revision and the nine static attachment names before the long
  build starts (a missed name now costs seconds, not 90 minutes), and
  \`scripts/ci-smoke-packages.sh\` installs the debs with apt, runs \`ldd\` on the
  installed paths and starts the packaged helper headless to read its sandbox
  banner.
- \`cefnest-build.tar.gz\` matches its own description: \`src/\`, \`examples/\` (every
  example) and \`test/\` keep the three directories the binaries' \`\$ORIGIN\` rpaths
  expect, and the library inside carries the same version the packages do. It
  holds **no helper and no CEF runtime**, and the notes say so.
- The runtime requirement is documented and in the package descriptions: the
  helper starts Chromium with the sandbox on and installs \`chrome-sandbox\` 0755 —
  no setuid bit, no AppArmor profile. On a host that restricts unprivileged user
  namespaces (Ubuntu 24.04 by default) it fails loudly at startup rather than
  silently dropping the sandbox. Two ways out: an AppArmor profile allowing
  \`userns\` for \`cefnest-helper\`, or \`CHROMIUM_HELPER_NO_SANDBOX=1\`.
- Licensing metadata: all four debs ship a machine-readable copyright at
  \`/usr/share/doc/<package>/copyright\`, the helper's records the CEF runtime as
  BSD-3-clause and installs \`LICENSE.cef.txt\`; the RPM helper subpackage declares
  \`License: LGPL-2.1-only and BSD-3-Clause\`.
- \`scripts/fetch-cef.sh\` pins the sha256 of both \`154.0.34\` archives in the
  repository: the downloaded bytes must equal the value this tree approved before
  anything is unpacked, instead of trusting only the digest the build service
  serves next to the file.
- Example fixes: zoom is shown as \`100 × 1.2^level\` (it was read as a linear
  percent, so one step in displayed "50%"); a context-menu popover parents to the
  view, not the window, because \`::context-menu\` x/y are view coordinates;
  collision-free download names keep their extension and no longer leak; the
  paint stack is freed; the find bar swallows Escape only while open; the mute
  icon follows the state.
- \`src/chromium-session.h\` corrected: closing a tab does emit
  \`::browser-closed\` — the previous API text still claimed it failed in silence.

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
