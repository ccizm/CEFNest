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

本版把 CEF 升到 156 稳定版（Chromium $CHROMIUM_VERSION），并带来 154 系列
之后落地的组件能力：

- **CEF 154 → 156**：全新里程碑。实测变化：Chromium 拖放不再提供
  \`chromium/x-web-custom-data\` 类型（跨边界拖放现在携带 2 种类型），
  dnd 门禁已按新行为校准；其余门禁（协议、会话脚本、焦点链、遮挡节流、
  打包）全量复跑通过。
- **查看网页源代码**：右键菜单的“查看网页源代码”不再写临时文件、不再拉起
  系统编辑器，而是组件自带的只读源码窗口——行号、语法高亮、查找、自动
  换行、复制全部；源码经控制通道分块流入，仅在内存中流转。
  新增 \`chromium_session_view_source()\`、\`::view-source\` 信号与
  \`auto-source-window\` 属性。运行时新增 GtkSourceView 5 依赖。
- **Tab 焦点链打通**：Tab/Shift+Tab 可以在 GTK 控件与页面元素之间完整穿行
  （页面边缘出让给 GTK 焦点链，进入页面落在首个可聚焦元素）。
- **性能与体积**：被完全遮挡的窗口节流到 2 帧/秒（心跳唤醒）；隐藏窗口
  停止产帧；dmabuf 缓冲推迟到被顶替时释放（消除撕裂风险）；CEF 运行时
  打包时 strip——安装体积从约 1.5 GB 降到约 350 MB。
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

This release moves CEF to the 156 stable channel (Chromium $CHROMIUM_VERSION)
and ships the component work that landed after the 154 series:

- **CEF 154 -> 156**, a new milestone. Measured change: Chromium drag-and-drop
  no longer offers the \`chromium/x-web-custom-data\` type (a cross-boundary
  drag now carries two types); the dnd gate is calibrated to the new
  behaviour, and the full gate battery (protocol, session scripts, focus
  chain, occlusion throttling, packaging) was re-run green.
- **View Source**: the context menu's View Source no longer writes a temp
  file and opens the desktop's editor — it is the component's own read-only
  window now, with line numbers, syntax highlighting, find, wrap and
  copy-all; the source streams over the control channel and stays in
  memory. New \`chromium_session_view_source()\`, \`::view-source\` and the
  \`auto-source-window\` property. GtkSourceView 5 is a new runtime
  dependency.
- **One focus chain**: Tab and Shift+Tab now cross between GTK widgets and
  page elements in a single chain (the page's edge hands the walk back to
  GTK; entering the page lands on its first focusable element).
- **Performance and size**: fully covered windows throttle to ~2 frames per
  second with a heartbeat to wake them; hidden windows stop producing
  frames; dmabuf buffers are held until replaced (no more tearing risk);
  and the CEF runtime ships stripped — installed size drops from ~1.5 GB to
  ~350 MB.
- Packaging stays two packages: \`cefnest\` (library + helper + CEF runtime)
  and \`cefnest-dev\` (headers, \`cefnest.pc\`).

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
