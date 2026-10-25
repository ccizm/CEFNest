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

# tzdata is dealt with on its own, before anything else can drag it into a
# transaction: see scripts/ci-prepare-tzdata.sh for why the bind-mounted
# /etc/localtime takes the whole apt run down with it.
$SUDO apt-get update
$SUDO ./scripts/ci-prepare-tzdata.sh

$SUDO apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config git linux-libc-dev \
  libgtk-4-dev libgtksourceview-5-dev libwayland-dev libxkbcommon-dev libjson-glib-dev \
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
# --buildtype=release: without it this tree inherits meson's debugoptimized
# (-O2 -g, assertions on), and everything shipped in cefnest-build.tar.gz —
# and every gate CI runs — would be measured on a non-release configuration
# while build-packages.sh configures its own tree as plain release.
meson setup builddir --buildtype=release -Dprefix=/usr -Dcefnest-version="$PACKAGE_VERSION"
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

本版在同一 CEF 156 稳定版上落地数据目录：默认不再向 ~/.cache 写任何
profile。

- **默认无痕、零落盘**：不设置路径时，helper 以 incognito 运行（HTTP 缓存
  在内存），CEF 必须写的安装级数据进 \$XDG_RUNTIME_DIR 下的 0700 临时
  目录——退出即删、重启即清。以往"每次会话在 ~/.cache 新建 3.5-8.7 MB 的
  profile、14 天后才被清理"的行为整体移除。
- **数据目录 API**：\`chromium_session_set_data_directory()\` 在 start() 前
  设一个绝对路径，cookies、localStorage、IndexedDB、HTTP 缓存全部落在该
  目录下并跨重启保留；\`chromium_session_clear_data()\` 一键清除，有会话
  占用时拒绝。同目录互斥由 \`<dir>/.cefnest.lock\` 的 flock 保证，崩溃自动
  释放、无陈旧锁。
- demo 新增 \`--data-dir\`；新增示例 \`examples/persistent.c\`；standalone
  helper 以 \`CHROMIUM_HELPER_DATA_DIR\` 获得同样的持久化。
- cefnest-dev 补上缺失的 \`libgtksourceview-5-dev\` 依赖（上一版流水线冒烟
  失败的原因），rpm devel 同步补 \`gtksourceview5-devel\`。
- 安装包形态沿用两包制：\`cefnest\`（库 + helper + CEF 运行时）、
  \`cefnest-dev\`（头文件与 \`cefnest.pc\`）。

### 附件说明

| 文件 | 说明 |
| --- | --- |
| cefnest-build.tar.gz | 构建树：\`src/\`（\`libcefnest.so\`、\`.so.0\`、\`.so.$PACKAGE_VERSION\`、\`cefnest-demo\`）、\`examples/\`（$EXAMPLE_COUNT 个示例）、\`test/\`（\`shm-client\`、\`seat-selftest\`）。解包后保持这三层目录，二进制会用自身的 \`\$ORIGIN\` rpath 找到同批的库。**不含 helper、不含 CEF 运行时**：装 \`cefnest\` 后，\`src/cefnest-demo\` 会直接找到安装位置里的 helper |
| cefnest_${V}_amd64.deb | GTK 4 组件库：\`libcefnest.so.0\` + \`cefnest-helper\` + 完整 CEF 运行时，运行时已 strip（安装后约 350 MB） |
| cefnest-dev_${V}_amd64.deb | 开发文件：三个头文件、链接用 \`.so\`、\`cefnest.pc\` |
| cefnest-$V.x86_64.rpm | GTK 4 组件库（rpm） |
| cefnest-devel-$V.x86_64.rpm | 开发文件（rpm） |
| SHA256SUMS | dist/ 下全部上传物的 SHA-256 清单，含 cefnest-build.tar.gz |

---

## English

This release lands the data directory on the same CEF 156 stable
(Chromium $CHROMIUM_VERSION): by default nothing is written to ~/.cache
any more.

- **Incognito by default, zero disk**: with no path set the helper runs
  incognito (HTTP cache in memory) and CEF's install-level files go to a
  0700 tmpfs directory under \$XDG_RUNTIME_DIR — removed on exit, gone on
  reboot. The old behaviour, a fresh 3.5-8.7 MB profile per session swept
  only after two weeks, is gone entirely.
- **A data-directory API**: set an absolute path before start() with
  \`chromium_session_set_data_directory()\` and everything the page stores
  — cookies, localStorage, IndexedDB, the HTTP cache — lives under it and
  survives restarts; \`chromium_session_clear_data()\` wipes it again and
  is refused while a session holds it. Exclusivity is a flock on
  \`<dir>/.cefnest.lock\`, crash-safe, no stale locks.
- The demo grows a \`--data-dir\` option, \`examples/persistent.c\` shows
  the lifecycle, and standalone helpers persist the same way through
  \`CHROMIUM_HELPER_DATA_DIR\`.
- cefnest-dev now depends on \`libgtksourceview-5-dev\` — the miss that
  failed the previous pipeline's smoke stage — and the devel rpm on
  \`gtksourceview5-devel\`.
- Packaging stays two packages: \`cefnest\` (library + helper + CEF
  runtime) and \`cefnest-dev\` (headers, \`cefnest.pc\`).

### Assets

| File | Description |
| --- | --- |
| cefnest-build.tar.gz | Build tree: \`src/\` (\`libcefnest.so\`, \`.so.0\`, \`.so.$PACKAGE_VERSION\`, \`cefnest-demo\`), \`examples/\` ($EXAMPLE_COUNT examples), \`test/\` (\`shm-client\`, \`seat-selftest\`). Keep the three directories when unpacking — the binaries find this same library through their own \`\$ORIGIN\` rpath. **No helper, no CEF runtime**: after installing \`cefnest\`, \`src/cefnest-demo\` finds the installed helper directly |
| cefnest_${V}_amd64.deb | GTK 4 component library: \`libcefnest.so.0\` + \`cefnest-helper\` + complete CEF runtime, stripped of debug sections (~350 MB installed) |
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
