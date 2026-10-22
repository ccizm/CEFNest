# CNB（cnb.cool）接入

仓库根目录的 `.cnb.yml` 现在做**编译产物和安装包**：在 CNB 托管 Ubuntu 24.04 节点上
安装编译依赖，拉取最新 stable CEF，构建 `cefnest-helper`，`meson setup` + `ninja`，
把库、demo、示例和测试客户端打包成 `cefnest-build.tar.gz`，再由
`scripts/build-packages.sh` 产出 deb 和 rpm，全部作为当前 commit 的附件上传。它不运行
`test/ci-all.sh`。

## 1. 为什么默认不是完整门禁

完整门禁需要：

- GTK 4 / Wayland / xkbcommon / json-glib / gbm / DRM 的开发包；
- `gnome-shell --headless` 和它需要的 D-Bus 会话；
- ImageMagick（`pixel-gate.sh`）。

build 流水线自己会拉 CEF（`third_party/cef/dist`，约 1.5 GB，不进 git）并构建
helper；它缺的是可用的显示/D-Bus 会话和 `gnome-shell --headless`。所以默认流水线
只编译和打包；完整门禁留在本地，或放在已经准备好的自托管 runner 上。

## 2. 产物里有什么

`cefnest-build.tar.gz` 包含：

- `libcefnest.so*`（含 soname 软链接）；
- `cefnest-demo`；
- `cefnest-host`、`cefnest-minimal`（如果该次构建生成了它们）；
- `shm-client`、`seat-selftest`。

不包含 `helper`，因为 helper 由 CMake 构建并需要 CEF；也不包含 `builddir` 的
对象文件。产物架构跟随 CNB 托管节点；解压后运行 demo 需要把 `libcefnest.so`
所在目录加进 `LD_LIBRARY_PATH`。

同一次构建还会在 `dist/` 下产出可安装的 Debian 和 RPM 包：

| 包 | 内容 | 用途 |
| --- | --- | --- |
| `libcefnest0` / `cefnest` | `libcefnest.so.0`、`libcefnest.so.154.0.34` 和一个 `shlibs` 声明 | 运行时依赖；`Recommends` helper |
| `libcefnest-dev` / `cefnest-devel` | 三个头文件、`libcefnest.so`、`cefnest.pc` | 用 `pkg-config` 编译宿主 |
| `cefnest-helper` | `cefnest-helper`、`libcef.so`、`icudtl.dat`、`locales/` 等 | 真实 Chromium 渲染，约 1.5 GB |
| `cefnest-demo` | `cefnest-demo` 和 `/usr/libexec/cefnest/shm-client` | 演示；依赖 helper |

包版本跟随构建时使用的 CEF stable 版本：`cefnest-version` = CEF 的 `X.Y.Z`
（当前实测 `154.0.34`），完整 CEF/Chromium 版本写进 `cefnest-helper` 的
`Description`。`scripts/fetch-cef.sh` 默认取最新 stable；要固定某个构建就设
`CEF_VERSION`，要取任意 channel 的最新构建就设 `CEF_CHANNEL=any`。

Debian 包按 `amd64`/`arm64` 的 multiarch 路径安装，RPM 包按 `/usr/lib64` 安装，
两者都由 `meson install` 的安装树直接打包，不移动文件。helper 包把整个 CEF runtime
放在 `/usr/libexec/cefnest/<arch>/`，`libcefnest0` 编译时烧进去的默认路径正是这里。

```bash
# 作为依赖使用：库、头文件、helper（helper 会拉入 CEF）
sudo apt install ./libcefnest0.deb ./libcefnest-dev.deb ./cefnest-helper.deb
# 或者
sudo dnf install ./cefnest.rpm ./cefnest-devel.rpm ./cefnest-helper.rpm

cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

只想验证合成器、不装 1.5 GB 的 CEF 时，装 `cefnest-demo` 但不装
`cefnest-helper`：demo 会回退到同一个包里的 `shm-client`。正常安装
`cefnest-demo` 会把 `cefnest-helper` 作为强依赖一起装上，所以 demo 默认能显示真实
网页。

`dist/` 里同时保留带版本号的标准文件名（`libcefnest0_154.0.34-1_amd64.deb` 等）、
`libcefnest0.deb` 这类固定别名、`cefnest-packages.tar.gz` 和 `SHA256SUMS`。固定别名
是给 CNB 附件列表用的：附件列表不展开 shell 变量，而版本号在构建时才确定。
`cefnest-packages.tar.gz` 只装小包；helper 的 deb/rpm 有几百 MB，作为单独附件上传。
RPM 的 `Requires` 按 Fedora 的包名写（`gtk4`、`libwayland-server`、`mesa-libgbm` 等），
helper 包的系统 soname 依赖由 `rpmbuild` 自动生成；其它 RPM 发行版如果包名不同，
需要调整 spec。

## 3. 完整门禁（本地或自托管 runner）

```bash
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libwayland-dev libxkbcommon-dev libjson-glib-dev \
  libgbm-dev libdrm-dev libvulkan-dev imagemagick gnome-shell
./scripts/fetch-cef.sh                 # 默认最新 stable；arm64 用 CEF_ARCH=aarch64
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j"$(nproc)"
meson setup builddir
ninja -C builddir
./test/ci-all.sh
```

`fetch-cef.sh` 默认取 CEF build service 上最新的 stable；要复现某次构建就设
`CEF_VERSION=<完整 CEF 版本>`。它把实际下载的版本写进
`third_party/cef/dist/.cef-version`，`scripts/check-cef-pin.sh` 用这个文件和
`include/cef_version.h` 比对，runner 上的旧分发包不会冒充新构建。`ci-all.sh` 每次
运行前会用 `cmake --build helper/build` 增量重建 helper。只想跑不依赖 helper 的检查
时，用 `CI_SKIP_HELPER_BUILD=1 ./test/ci-all.sh unit`。

如果要把完整门禁搬回 CNB，给自托管节点打上与 `.cnb.yml` 的 `runner.tags` 完全
一致的标签（例如 `cnb:arch:default` + `cefnest-gate`），并把 stage 的 `script`
换成 `CI_LOG_DIR=ci-logs ./test/ci-all.sh 2>&1 | tee ci-logs/summary.txt`。
`ci-logs` 可以用 `cnbcool/attachments` 插件上传到 commit 附件。

## 4. 超时与附件

`build` job 设置了 `timeout: 90m`。CNB 没有流水线级 timeout，只有 job 级；这个
值约束 CEF 下载、helper 编译、三份源码树（`builddir`、`build-pkg-deb`、
`build-pkg-rpm`）和两次大包压缩，不约束完整门禁。`upload build` stage 用
`cnbcool/attachments:latest` 把 `cefnest-build.tar.gz`、八个 deb/rpm、
`cefnest-packages.tar.gz` 和 `SHA256SUMS` 放到当前 commit 的附件；上传失败会让
流水线失败，但产物仍留在该次构建的工作区里。
