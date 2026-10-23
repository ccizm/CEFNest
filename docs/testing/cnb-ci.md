# CNB（cnb.cool）接入

仓库根目录的 `.cnb.yml` 只在**发版**时构建：推一个版本 tag（如 `v154.0.34-6`）才触发，
普通分支推送不触发任何构建——门禁留在本地或自托管 runner 上（§3）。四个 stage 依次是：
先比对版本串与 tag（`scripts/check-release-tag.sh`，见 §4）；然后在 CNB 托管 Ubuntu 24.04
节点上装编译依赖、拉取已验证的 stable CEF 154.0.34、构建 `cefnest-helper`、`meson setup
-Dcefnest-version=…` + `ninja`，把库、demo、全部示例和测试客户端按构建目录布局打成
`cefnest-build.tar.gz`，再由 `scripts/build-packages.sh` 产出带版本号的 deb 和 rpm
（`scripts/ci-build.sh` 同时生成 `RELEASE-NOTES.md`）；接着把 `dist/` 里的包真装一遍并冒烟
（§4.1）；最后创建该 tag 的 Release 并把全部产物作为 Release 资产上传。它不运行
`test/ci-all.sh`，也不把 helper 或 CEF 运行时放进 `cefnest-build.tar.gz`。

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

`cefnest-build.tar.gz`（`dist/` 里，Release 资产从那里上传）保持构建目录的三层布局：

- `src/`：`libcefnest.so`、`libcefnest.so.0`、`libcefnest.so.<X.Y.Z>` 与 `cefnest-demo`；
- `examples/`：全部 13 个示例可执行文件；
- `test/`：`shm-client`、`seat-selftest`。

布局是判据不是美观：demo 的 rpath 是 `$ORIGIN/`，示例与测试客户端是
`$ORIGIN/../src`。摊平成一个目录之后这些 rpath 全部指向不存在的地方，"解压即可运行"
就悄悄变成"解压后请设 `LD_LIBRARY_PATH`"。库版本串由同一次 `meson setup
-Dcefnest-version=<X.Y.Z>` 决定，和包名里的版本是同一个串——不传这个选项时 meson 会
用仓库默认值 `0.0.1`，于是同一个 Release 里出现两个版本号。

不包含 `helper`，也不包含 CEF 运行时（那是 1.5 GB 的 `cefnest-helper` 包）。要在这里
渲染页面，装下面的包，或者用 `CHROMIUM_HELPER` 指到一个已构建的 helper。

同一次构建还会在 `dist/` 下产出可安装的 Debian 和 RPM 包：

| 包 | 内容 | 用途 |
| --- | --- | --- |
| `cefnest-helper` | `cefnest-helper`、`libcef.so`、`icudtl.dat`、`locales/` 等 | 真实 Chromium 渲染，约 1.5 GB；底层包，只依赖系统库 |
| `libcefnest0` / `cefnest` | `libcefnest.so.0`、`libcefnest.so.154.0.34` 和一个 `shlibs` 声明 | 运行时依赖；精确依赖 `cefnest-helper (= 版本-修订)` |
| `libcefnest-dev` / `cefnest-devel` | 三个头文件、`libcefnest.so`、`cefnest.pc` | 用 `pkg-config` 编译宿主；精确依赖 `libcefnest0` |
| `cefnest-demo` | `cefnest-demo` 和 `/usr/libexec/cefnest/shm-client` | 演示；精确依赖 `libcefnest0` 与 `cefnest-helper` |

依赖图严格分层、无循环：helper（CEF 运行时）→ `libcefnest0` → `-dev` / demo。
四个包共用一个版本号——CEF stable `X.Y.Z` 加修订号（`154.0.34-6`）——从同一源码树
同一次构建产出、同一个 tag 发布，升级时整体联动；单独升级某一个不可行，这正是
lib 与 helper 之间私有控制协议想要的保护。包版本跟随构建时使用的 CEF stable 版本：
`cefnest-version` = CEF 的 `X.Y.Z`（当前实测 `154.0.34`），完整 CEF/Chromium 版本
写进 `cefnest-helper` 的 `Description`。同版本重发用修订号区分：流水线导出
`CEFNEST_PACKAGE_RELEASE`（`.cnb.yml` 里当前为 `6`），产物名形如
`libcefnest0_154.0.34-6_amd64.deb`；包管理器把 `-5` 和 `-6` 视为不同版本，
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
sudo apt install ./cefnest-demo_154.0.34-6_amd64.deb
# 或者按层安装
sudo apt install ./cefnest-helper_154.0.34-6_amd64.deb \
  ./libcefnest0_154.0.34-6_amd64.deb ./libcefnest-dev_154.0.34-6_amd64.deb
# RPM 系（demo 同样会把库与 helper 拉上）
sudo dnf install ./cefnest-demo-154.0.34-6.x86_64.rpm

cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

`shm-client` 回退只存在于源码构建：包安装里 `libcefnest0` 硬依赖 helper，
装库必然带上 1.5 GB 的 CEF 运行时。想在无 CEF 的环境演练合成器，用方法二的
开发构建（`docs/guides/packaging.md`），demo 会退而驱动 `test/shm-client`。

`dist/` 里的产物全部带版本-修订号（`libcefnest0_154.0.34-6_amd64.deb` 等），外加
完整构建树 `cefnest-build.tar.gz` 和 `SHA256SUMS`。CNB 的附件列表不展开 shell
变量，所以 `.cnb.yml` 里的附件名是静态写死的——升版本或修订号时要同步手改。
RPM 的 `Requires` 按 Fedora 的包名**手写**（`gtk4`、`libwayland-server`、`mesa-libgbm`
等，helper 子包只有一行 `glibc >= 2.39`）。spec 里的 `%global __os_install_post %{nil}`
关掉的是 brp 后处理（strip、压缩 man、`%{_build_id_links}` 一类），**不是**
`find-requires` 那条依赖抽取——所以 helper 包里 `libcef.so` 的 soname 依赖到底会不会
自动出现在 `rpm -qp --requires` 里，本仓库还没有一台真正跑过 rpmbuild 的宿主量过它。
`scripts/ci-smoke-packages.sh` 因此把这一行**打印**出来而不做断言（红或绿都不该由猜
决定），第一次 CI 跑完应当把结果补回这一段。deb 侧不存在这个不确定性：
`dpkg-shlibdeps` 直接对 `libcef.so` 与 helper 生成 `Depends`，冒烟 job 再用 apt 真装一遍。
其它 RPM 发行版如果包名不同，需要调整 spec。

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

`./scripts/fetch-cef.sh` 默认取已验证的 stable 154.0.34；要试最新 stable 就设
`CEF_VERSION=latest`，要复现某次构建就设 `CEF_VERSION=<完整 CEF 版本>`。它把实际下载的版本写进
`third_party/cef/dist/.cef-version`，`scripts/check-cef-pin.sh` 把**三方**放在一起比：
`fetch-cef.sh` 里的 `PINNED`（一次全新下载会得到什么）、这份记录、以及
`include/cef_version.h`（helper 实际编译所依据的版本）。只比后两者是没有用的——那两个文件
同一次 `fetch-cef.sh` 写的，最多能发现 dist/ 被换了一半，永远发现不了"这台 runner 当年是按
另一个版本准备的"。故意 `CEF_VERSION=latest` 的机器会在这里失败，这是要的：跑完整门禁时要么把
pinned 版本拉回来，要么明说 `CI_SKIP_HELPER_BUILD=1 ./test/ci-all.sh unit`（连 helper 增量重建
一起放弃）。`ci-all.sh` 每次运行前会用 `cmake --build helper/build` 增量重建 helper。

如果要把完整门禁搬回 CNB，给自托管节点打上与 `.cnb.yml` 的 `runner.tags` 完全
一致的标签（例如 `cnb:arch:default` + `cefnest-gate`），并把 stage 的 `script`
换成 `CI_LOG_DIR=ci-logs ./test/ci-all.sh 2>&1 | tee ci-logs/summary.txt`。
`ci-logs` 可以用 `cnbcool/attachments` 插件上传到 commit 附件。

## 4. 超时与附件

发版流水线是四个 stage 串在一起，顺序有讲究：

| stage | 跑什么 | timeout |
| --- | --- | --- |
| `check the release strings` | `scripts/check-release-tag.sh`：`PINNED` × `CEFNEST_PACKAGE_RELEASE` × `.cnb.yml` 的 `CEFNEST_EXPECTED_TAG` 与静态附件名必须互相一致 | 10m |
| `build` | `scripts/ci-build.sh`：apt 依赖、`fetch-cef.sh`、helper、`builddir`（带 `-Dcefnest-version`）、`dist/` 与 `RELEASE-NOTES.md` | 90m |
| `install and smoke the packages` | `scripts/ci-smoke-packages.sh`：见 §4.1 | 30m |
| `create release` + `upload release assets` | `git:release` 用 `RELEASE-NOTES.md` 建 Release，`cnbcool/attachments` 上传 `dist/` 里那 11 个名字 | — |

第一个 stage 存在的理由很简单：附件名是静态的，漏改一个是 90 分钟后才炸；把检查提到
最前面，同一件事几秒钟就有结论。它只能检查流水线**告诉它**的那个 tag（`CEFNEST_EXPECTED_TAG`
写在 `.cnb.yml` 里，和它检查的正是同一张清单）；runner 的 checkout 里有没有 tag ref 不可靠，
所以 `git tag --points-at HEAD` 有结果时一并比，没有就照实说一句。

CNB 没有流水线级 timeout，只有 job 级；`build` 那 90 分钟约束的是 CEF 下载、helper 编译、
三份源码树（`builddir`、`build-pkg-deb`、`build-pkg-rpm`）和两次大包压缩，不约束完整门禁。
`upload release assets` stage 用 `cnbcool/attachments:latest` 把 `dist/cefnest-build.tar.gz`、
八个 deb/rpm（四个包 × deb/rpm）、`cefnest-packages-<版本>-<修订>.tar.gz` 和 `SHA256SUMS`
放到该 tag 的 Release；上传失败会让流水线失败，但产物仍留在该次构建的工作区里。

### 4.1 装包冒烟

发版过去只*生产*包：四个 deb 的 `Depends` 是手写的，没人证明过它们的名字存在；
1.5 GB 的 CEF 运行时是不是真跟着 helper 落到了 `/usr/libexec/cefnest/<arch>/`，也没人
在装完之后看过。`scripts/ci-smoke-packages.sh` 补的就是这两段，按能在容器里做到的和做不到的
分得很清楚：

- `sha256sum -c dist/SHA256SUMS`，并要求清单里有 `cefnest-build.tar.gz`（清单只覆盖
  `build-packages.sh` 自己产出的那些文件时，"全部产物的校验清单"这句话是假的）；
- `dpkg -c` 逐个点名 helper 包里的运行文件、`libcefnest0` 里那个带版本的 `.so`，
  以及 `chrome-sandbox` 的权限位必须是 `-rwxr-xr-x`（0755、非 setuid 是决定，写成判据
  才不会被以后某次"顺手修权限"改掉）；
- 四个包都必须把 machine-readable copyright 放在**数据层**的
  `/usr/share/doc/<包名>/copyright`，helper 那份还要有一段 `Files: usr/libexec/cefnest/*`
  的 BSD-3-clause。这一条是踩出来的：第一版写的是 `DEBIAN/copyright`，看着像有了，其实
  dpkg 根本不会安装控制层里的这个成员，`lintian` 依旧报 `E: no-copyright-file`。判据直接
  从 `.deb` 的数据流里 `tar -xO` 那一个成员（一次 1.5 GB 的读流，十几秒），所以不装包也能量到；
  把改动退回 `DEBIAN/copyright` 重跑，这一条变红。
- 以 root `apt-get install ./dist/*.deb`：这一条是手写 `Depends` 唯一的真判据，名字写错、
  版本约束写错、依赖一个归档里不存在的包，都在这里当场失败；
- 装完之后对 `/usr/lib/*/libcefnest.so.0` 与安装路径上的 `cefnest-helper` 跑 `ldd`，
  任何 `not found` 都算失败；
- 不带显示直接起一次 helper，读它的横幅：`helper: sandbox=on` 必须在（打包默认开沙箱），
  出现 `No usable sandbox!` 就是这次包在这个宿主上根本跑不起来。这条**不**能替你测
  Ubuntu 24.04 那套 AppArmor 限制（`apparmor_restrict_unprivileged_userns` 默认开）——容器复现不了别人机器上的策略，能复现的是"默认值
  到底是什么、以及响亮失败长什么样"（`docs/internals/sandbox-policy.md` §4 是量法）。
- RPM 侧只 `rpm -qlp`/`-qp`：冒烟 job 在 Debian 容器里，不会去装一个 RPM 数据库。

不在 root 时安装那一段会被跳过，而且会跳过得有声有色：不给 `CI_SMOKE_SKIP_INSTALL=1`
就直接退出失败，免得一条只做了 inspect 的绿线被读成"包装过了"。

## 5. 发版（tag push）

普通分支推送不触发任何构建。发版推一个与包版本-修订一致的 tag：

```bash
git tag v154.0.34-6
git push origin v154.0.34-6
```

`.cnb.yml` 的 `$: tag_push` 按 §4 那四个 stage 依次跑：`scripts/check-release-tag.sh`
先比版本串与 tag，`scripts/ci-build.sh` 才做重活（导出 `CEFNEST_PACKAGE_RELEASE=6`，
构建时生成中英双语的 `RELEASE-NOTES.md`，含“本版变更”清单与附件说明表格），
`scripts/ci-smoke-packages.sh` 把 `dist/` 里的包装一遍，然后：

- `git:release` 用该 tag 创建 Release，描述从 `RELEASE-NOTES.md` 读取；
- `cnbcool/attachments` 把 `dist/` 里那 11 个文件上传到该 tag 的 Release。

顺序不能反：附件插件要求 Release 已经存在。重发同版本时要递增修订号，而 `.cnb.yml`
里跟着动的有 **13 个位置**：三行 `CEFNEST_PACKAGE_RELEASE=6`（check、build、smoke
各一处）、一行 `CEFNEST_EXPECTED_TAG=v154.0.34-6`、以及 9 个带版本的静态附件名——
`cefnest-build.tar.gz` 与 `SHA256SUMS` 这两个名字永远不带版本。附件列表不展开 shell
变量，所以这份清单只能手写；上面第一个 stage 就是为它手写清单兜底的，漏改哪一处都在
几十秒内失败，而不是 90 分钟后少一个文件。tag 名要与包版本一致（`v154.0.34-6`），
包版本本身仍由 CEF 的 `X.Y.Z` 决定。

`create release` 带着 `latest: true`，而 CNB 没有"只有最新的 tag 才标 latest"这种条件：
重推一个旧 tag 会让旧版本抢走 latest 标记。发版顺序保持向前即可避开；要回退标记只能
在页面上改。
