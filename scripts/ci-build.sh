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

本版变更（相对 154.0.34-5）：

- \`wl_shm\` 的请求槽 1 现在有实现，全局版本升到 2：Wayland 1.22 上客户端正常销毁
  自己的 \`wl_shm\` 代理不再命中空槽，把嵌套合成器连同宿主一起 \`wl_abort\`。源码
  审计把显式 \`NULL\`/\`0\` 初始化式也算作空槽；新运行时门禁
  \`seat-selftest-shm-release\` 发一条裸请求并要求连接继续被服务，\`--wrong-object\`
  的帧头也按真实布局（对象 id 在前）修正。
- CEF 子进程改用 \`_exit(code)\` 离开 \`main()\`，不再走栈保护尾言。CEF 154 的
  fork-without-exec 子进程继承父进程的 canary 槽，正常返回会打出
  \`*** stack smashing detected ***\` 并以 SIGABRT 收尾；\`test/ci-session.sh\`
  已把这类行从“容忍的沙箱噪声”改成失败。
- helper 把“页面没了”和“进程没了”分成两个时刻：\`::closed\` 仍由页面关闭发出，
  新增的 \`::helper-exited\` 在 \`child_waited()\` 收走子进程之后才发。宿主若拥有
  helper 正在画的嵌套 compositor，就该等后者再拆——\`systemctl stop\`/注销时
  helper 先报 \`closed\`、再在 \`CefShutdown()\` 里继续用那条 Wayland 连接，先拆
  会让它的 GDK 打出 \`Lost connection to Wayland compositor\` 并把进程结束在
  \`main()\` 尾部之前。demo 的脚本在 helper 主动退出时据此推迟退出，
  \`test/helper-signal-gate.sh\` 的 \`load\`/\`wait\`/\`quit\` 因此保留 pending 步并
  真正走到这条路径；每边 9 条断言，改完连跑 10 次全绿（此前 11 次里红 2 次）。

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

Highlights of this release (relative to 154.0.34-5):

- Request slot 1 of \`wl_shm\` is implemented and the global is advertised at
  version 2: on Wayland 1.22 a client that destroys its own \`wl_shm\` proxy no
  longer hits an empty slot and \`wl_abort\`s the nested compositor together with
  the host. The source audit now counts explicit \`NULL\`/\`0\` initializers as
  empty slots, a new runtime gate (\`seat-selftest-shm-release\`) sends the raw
  request and requires the connection to keep being served, and the
  \`--wrong-object\` frame header was corrected to the real layout (object id
  first).
- CEF subprocesses leave \`main()\` with \`_exit(code)\` instead of returning
  through the stack protector. CEF 154's fork-without-exec children inherit the
  parent's canary slot, so a normal return printed
  \`*** stack smashing detected ***\` and ended in SIGABRT; \`test/ci-session.sh\`
  now treats that line as a failure instead of a tolerated sandbox message.
- The helper now separates "the page is gone" from "the process is gone":
  \`::closed\` still comes when the browser closes, and the new
  \`::helper-exited\` follows once \`child_waited()\` has reaped the child. A host
  that owns the nested compositor the helper draws into should tear it down on
  the latter — on \`systemctl stop\` or a logout the helper reports \`closed\`
  and then spends \`CefShutdown()\` still using that Wayland connection, so
  tearing the display down first makes its GDK print
  \`Lost connection to Wayland compositor\` and end the process before the tail
  of \`main()\`. The demo now defers its scripted quit, and
  \`test/helper-signal-gate.sh\` keeps a pending \`wait\`/\`quit\` step so it
  exercises that path; nine assertions per signal, 10 consecutive green runs
  after the fix (the old shape failed 2 of 11).

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
