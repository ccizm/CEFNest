# CNB（cnb.cool）接入

仓库根目录的 `.cnb.yml` 只在**发版**时构建：推一个版本 tag（如 `v154.0.34-4`）才触发，
普通分支推送不触发任何构建——门禁留在本地或自托管 runner 上（§3）。CNB 托管
Ubuntu 24.04 节点上安装编译依赖，拉取已验证的 stable CEF 154.0.34，构建
`cefnest-helper`，`meson setup` + `ninja`，把库、demo、示例和测试客户端打包成
`cefnest-build.tar.gz`，再由 `scripts/build-packages.sh` 产出带版本号的 deb 和 rpm，
创建该 tag 的 Release 并把全部产物作为 Release 资产上传。它不运行 `test/ci-all.sh`。

## 1. 为什么默认不是完整门禁

完整门禁需要：

- GTK 4 / Wayland / xkbcommon / json-glib / gbm / DRM 的开发包；
- `gnome-shell --headless` 和它需要的 D-Bus 会话；
- ImageMagick（`pixel-gate.sh`）。

build 流水线自己会拉 CEF（`third_party/cef/dist`，约 1.5 GB，不进 git）并构建
helper；它缺的是可用的显示/D-Bus 会话和 `gnome-shell --headless`。所以默认流水线
只编译和打包；完整门禁留在本地，或放在已经准备好的自托管 runner 上。

apt 步骤在装包前设置 `DEBIAN_FRONTEND=noninteractive` 和 `TZ=Etc/UTC`，并向
debconf preseed `tzdata/Areas=Etc`、`tzdata/Zones/Etc=UTC`。随后**单独处理
`tzdata`**，最后才装其余依赖；失败时是一包失败、能拿到它自己的 stderr，而不是整条
python3/GTK 依赖链一起报错。

容器里的 `/etc/localtime` 通常是宿主 bind mount，`tzdata` 的 postinst 会用
`mv /etc/localtime.dpkg-new /etc/localtime` 覆盖它，而 rename 一个挂载点是
`EBUSY`。流水线先尝试 `umount /etc/localtime`；如果 runner 不允许，就把 tzdata
`dpkg --unpack`，只把 postinst 里那一段 rename 改成跳过，再 `dpkg --configure
tzdata`。`/etc/timezone` 仍会被写入，后续命令用导出的 `TZ`，不依赖
`/etc/localtime`。之后跑 `dpkg --configure -a` 和 `apt-get -f install -y` 收尾。

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
| `cefnest-helper` | `cefnest-helper`、`libcef.so`、`icudtl.dat`、`locales/` 等 | 真实 Chromium 渲染，约 1.5 GB；底层包，只依赖系统库 |
| `libcefnest0` / `cefnest` | `libcefnest.so.0`、`libcefnest.so.154.0.34` 和一个 `shlibs` 声明 | 运行时依赖；精确依赖 `cefnest-helper (= 版本-修订)` |
| `libcefnest-dev` / `cefnest-devel` | 三个头文件、`libcefnest.so`、`cefnest.pc` | 用 `pkg-config` 编译宿主；精确依赖 `libcefnest0` |
| `cefnest-demo` | `cefnest-demo` 和 `/usr/libexec/cefnest/shm-client` | 演示；精确依赖 `libcefnest0` 与 `cefnest-helper` |

依赖图严格分层、无循环：helper（CEF 运行时）→ `libcefnest0` → `-dev` / demo。
四个包共用一个版本号——CEF stable `X.Y.Z` 加修订号（`154.0.34-4`）——从同一源码树
同一次构建产出、同一个 tag 发布，升级时整体联动；单独升级某一个不可行，这正是
lib 与 helper 之间私有控制协议想要的保护。包版本跟随构建时使用的 CEF stable 版本：
`cefnest-version` = CEF 的 `X.Y.Z`（当前实测 `154.0.34`），完整 CEF/Chromium 版本
写进 `cefnest-helper` 的 `Description`。同版本重发用修订号区分：流水线导出
`CEFNEST_PACKAGE_RELEASE`（`.cnb.yml` 里当前为 `4`），产物名形如
`libcefnest0_154.0.34-4_amd64.deb`；包管理器把 `-4` 和 `-5` 视为不同版本，
重发必须递增修订号。`scripts/fetch-cef.sh` 默认取**已验证的** stable
`154.0.34`；`CEF_VERSION=latest` 取 CEF index 当前标为 stable 的最新构建，
`CEF_VERSION=<完整版本>` 固定其它构建，`CEF_CHANNEL=any` 取任意 channel 的最新构建。
CEF index 会把同一个构建同时列在 beta 和 stable 下（156.0.7 就是这样），所以默认不
盲目跟随“最新 stable”，避免未重跑门禁的新 major 自动改掉包版本。

Debian 包按 `amd64`/`arm64` 的 multiarch 路径安装，RPM 包按 `/usr/lib64` 安装，
两者都由 `meson install` 的安装树直接打包，不移动文件。helper 包把整个 CEF runtime
放在 `/usr/libexec/cefnest/<arch>/`，`libcefnest0` 编译时烧进去的默认路径正是这里。

```bash
# 装一个 cefnest-demo 就够：包管理器按 helper → 库 → demo 自动解算拉齐
sudo apt install ./cefnest-demo_154.0.34-4_amd64.deb
# 或者按层安装
sudo apt install ./cefnest-helper_154.0.34-4_amd64.deb \
  ./libcefnest0_154.0.34-4_amd64.deb ./libcefnest-dev_154.0.34-4_amd64.deb
# RPM 系（demo 同样会把库与 helper 拉上）
sudo dnf install ./cefnest-demo-154.0.34-4.x86_64.rpm

cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

`shm-client` 回退只存在于源码构建：包安装里 `libcefnest0` 硬依赖 helper，
装库必然带上 1.5 GB 的 CEF 运行时。想在无 CEF 的环境演练合成器，用方法二的
开发构建（`docs/guides/packaging.md`），demo 会退而驱动 `test/shm-client`。

`dist/` 里的产物全部带版本-修订号（`libcefnest0_154.0.34-4_amd64.deb` 等），外加
完整构建树 `cefnest-build.tar.gz` 和 `SHA256SUMS`。CNB 的附件列表不展开 shell
变量，所以 `.cnb.yml` 里的附件名是静态写死的——升版本或修订号时要同步手改。
RPM 的 `Requires` 按 Fedora 的包名写（`gtk4`、`libwayland-server`、`mesa-libgbm` 等），
helper 包的系统 soname 依赖由 `rpmbuild` 自动生成；其它 RPM 发行版如果包名不同，
需要调整 spec。

## 3. 完整门禁（本地或自托管 runner）

```bash
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libwayland-dev libxkbcommon-dev libjson-glib-dev \
  libgbm-dev libdrm-dev libvulkan-dev imagemagick gnome-shell
./scripts/fetch-cef.sh                 # 默认 154.0.34；CEF_VERSION=latest 取最新 stable
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j"$(nproc)"
meson setup builddir
ninja -C builddir
./test/ci-all.sh
```

`fetch-cef.sh` 默认取已验证的 stable 154.0.34；要试最新 stable 就设
`CEF_VERSION=latest`，要复现某次构建就设 `CEF_VERSION=<完整 CEF 版本>`。它把实际下载的版本写进
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
`build-pkg-rpm`）和两次大包压缩，不约束完整门禁。`upload release assets` stage 用
`cnbcool/attachments:latest` 把 `cefnest-build.tar.gz`、八个 deb/rpm（四个包 ×
deb/rpm）、`cefnest-packages-<版本>-<修订>.tar.gz` 和 `SHA256SUMS` 放到该 tag 的
Release；上传失败会让流水线失败，但产物仍留在该次构建的工作区里。

## 5. 发版（tag push）

普通分支推送不触发任何构建。发版推一个与包版本-修订一致的 tag：

```bash
git tag v154.0.34-4
git push origin v154.0.34-4
```

`.cnb.yml` 的 `$: tag_push` 会跑 `scripts/ci-build.sh`（流水线导出
`CEFNEST_PACKAGE_RELEASE=4`，构建时生成中英双语的 `RELEASE-NOTES.md`，含
“本版变更”清单与附件说明表格），然后：

- `git:release` 用该 tag 创建 Release，描述从 `RELEASE-NOTES.md` 读取；
- `cnbcool/attachments` 把同一批文件上传到该 tag 的 Release。

顺序不能反：附件插件要求 Release 已经存在。重发同版本时要递增修订号，并同步改
`.cnb.yml` 里的两处：`CEFNEST_PACKAGE_RELEASE` 导出值与静态附件名列表——附件
列表不展开 shell 变量。tag 名建议与包版本一致（`v154.0.34-4`），包版本本身仍由
CEF 的 `X.Y.Z` 决定。
