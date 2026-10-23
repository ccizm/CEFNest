# CNB（cnb.cool）接入

仓库根目录的 `.cnb.yml` 只在**发版**时构建：推一个版本 tag（如 `v154.0.34-7`）才触发，
普通分支推送不触发任何构建——门禁留在本地或自托管 runner 上（§3）。流水线依次是：
先比对版本串与 tag（`scripts/check-release-tag.sh`，见 §4）；然后在 CNB 托管 Ubuntu 24.04
节点上装编译依赖、拉取已验证的 stable CEF 154.0.34、构建 `cefnest-helper`、`meson setup
-Dprefix=/usr -Dcefnest-version=…` + `ninja`，把库、demo、全部示例和测试客户端按构建目录布局
打成 `cefnest-build.tar.gz`，再由 `scripts/build-packages.sh` 产出带版本号的两个 deb 和两个
rpm（`scripts/ci-build.sh` 同时生成 `RELEASE-NOTES.md`）；接着把 `dist/` 里的包真装一遍并冒烟
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
用仓库默认值 `0.0.1`，于是同一个 Release 里出现两个版本号。构建树同时用
`-Dprefix=/usr` 配置：不指定 prefix 时默认是 `/usr/local`，解包后的 demo 会去那里找
helper，而包把 helper 装在 `/usr/libexec/cefnest/<arch>/`。

不包含 `helper`，也不包含 CEF 运行时（它们是 `cefnest` 包的一部分）。要在这里
渲染页面，装下面的包，或者用 `CHROMIUM_HELPER` 指到一个已构建的 helper。demo 与
`shm-client` 只在这个归档里，不单独发包。

同一次构建还会在 `dist/` 下产出可安装的 Debian 和 RPM 包：

| 包 | 内容 | 用途 |
| --- | --- | --- |
| `cefnest`（deb）/ `cefnest`（rpm） | `libcefnest.so.0`、`libcefnest.so.154.0.34`、`shlibs`，以及 `/usr/libexec/cefnest/<arch>/` 下的 `cefnest-helper`、`libcef.so`、`icudtl.dat`、`locales/` 等 | GTK 4 组件库：嵌套合成器、部件、控制通道，连同 helper 与完整 CEF 运行时，约 1.5 GB；以 `libcefnest.so.0` 装成系统库，只依赖系统库 |
| `cefnest-dev`（deb）/ `cefnest-devel`（rpm） | 三个头文件、`libcefnest.so`、`cefnest.pc` | 用 `pkg-config` 编译宿主；精确依赖 `cefnest` |

依赖只有一层：`cefnest-dev` 精确依赖对应版本的 `cefnest`，组件库不再拆出单独的
helper 包——helper 与库之间的私有控制协议要求它们同批构建，而分开只会让每次装库
都仍然下载那 1.5 GB。两个包共用一个版本号——CEF stable `X.Y.Z` 加修订号
（`154.0.34-7`）——从同一源码树同一次构建产出、同一个 tag 发布，升级时整体联动。
包版本跟随构建时使用的 CEF stable 版本：`cefnest-version` = CEF 的 `X.Y.Z`
（当前实测 `154.0.34`），完整 CEF/Chromium 版本写进 `cefnest` 的 `Description`。
同版本重发用修订号区分：流水线导出 `CEFNEST_PACKAGE_RELEASE`（`.cnb.yml` 里当前为
`7`），产物名形如 `cefnest_154.0.34-7_amd64.deb`；包管理器把 `-6` 和 `-7` 视为
不同版本，重发必须递增修订号。`scripts/fetch-cef.sh` 默认取**已验证的** stable
`154.0.34`；`CEF_VERSION=latest` 取 CEF index 当前标为 stable 的最新构建，
`CEF_VERSION=<完整版本>` 固定其它构建，`CEF_CHANNEL=any` 取任意 channel 的最新构建。
CEF index 会把同一个构建同时列在 beta 和 stable 下（156.0.7 就是这样），所以默认不
盲目跟随“最新 stable”，避免未重跑门禁的新 major 自动改掉包版本。

Debian 包按 `amd64`/`arm64` 的 multiarch 路径安装，RPM 包按 `/usr/lib64` 安装，
两者都由 `meson install` 的安装树直接打包，不移动文件。`cefnest` 把整个 CEF runtime
放在 `/usr/libexec/cefnest/<arch>/`，库编译时烧进去的默认 helper 路径正是这里。

```bash
# 只运行宿主：一个文件就够，helper 与 CEF 都在里面
sudo apt install ./cefnest_154.0.34-7_amd64.deb
# 还要编译宿主：把开发包一并给出（apt 不会自动搜索同目录的本地包）
sudo apt install ./cefnest_154.0.34-7_amd64.deb ./cefnest-dev_154.0.34-7_amd64.deb
# RPM 系同理
sudo dnf install ./cefnest-154.0.34-7.x86_64.rpm ./cefnest-devel-154.0.34-7.x86_64.rpm

cc host.c -o host $(pkg-config --cflags --libs cefnest)
```

`shm-client` 回退只存在于源码构建：发布包只有 `cefnest`，装它必然带上 1.5 GB 的
CEF 运行时。想在无 CEF 的环境演练合成器，用方法二的开发构建
（`docs/guides/packaging.md`），demo 会退而驱动 `test/shm-client`；demo 与
`shm-client` 本身只在 `cefnest-build.tar.gz` 里。

`dist/` 里有六个上传物：两个包各两种格式（`cefnest_154.0.34-7_amd64.deb` 等）、
完整构建树 `cefnest-build.tar.gz` 和 `SHA256SUMS`。CNB 的附件列表不展开 shell
变量，所以 `.cnb.yml` 里的附件名是静态写死的——升版本或修订号时要同步手改。
RPM 的 `Requires` 按 Fedora 的包名**手写**（`gtk4`、`libwayland-server`、`mesa-libgbm`
等，另加一行 `glibc >= 2.39`；helper 子包已经不存在）。spec 里的
`%global __os_install_post %{nil}` 关掉的是 brp 后处理（strip、压缩 man、
`%{_build_id_links}` 一类），**不是** `find-requires` 那条依赖抽取——所以主包里
`libcef.so` 的 soname 依赖到底会不会自动出现在 `rpm -qp --requires` 里，本仓库还没有
一台真正跑过 rpmbuild 的宿主量过它。
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

发版流水线是几个 stage 串在一起，顺序有讲究：

| stage | 跑什么 | timeout |
| --- | --- | --- |
| `check the release strings` | `scripts/check-release-tag.sh`：`PINNED` × `CEFNEST_PACKAGE_RELEASE` × `.cnb.yml` 的 `CEFNEST_EXPECTED_TAG` 与静态附件名必须互相一致 | 10m |
| `build` | `scripts/ci-build.sh`：apt 依赖、`fetch-cef.sh`、helper、`builddir`（带 `-Dprefix=/usr` 与 `-Dcefnest-version`）、`dist/` 与 `RELEASE-NOTES.md` | 90m |
| `install and smoke the packages` | `scripts/ci-smoke-packages.sh`：见 §4.1 | 30m |
| `create release` + `upload release assets` | `git:release` 用 `RELEASE-NOTES.md` 建 Release，`cnbcool/attachments` 上传 `dist/` 里那 6 个名字 | — |

第一个 stage 存在的理由很简单：附件名是静态的，漏改一个是 90 分钟后才炸；把检查提到
最前面，同一件事几秒钟就有结论。它只能检查流水线**告诉它**的那个 tag（`CEFNEST_EXPECTED_TAG`
写在 `.cnb.yml` 里，和它检查的正是同一张清单）；runner 的 checkout 里有没有 tag ref 不可靠，
所以 `git tag --points-at HEAD` 有结果时一并比，没有就照实说一句。

CNB 没有流水线级 timeout，只有 job 级；`build` 那 90 分钟约束的是 CEF 下载、helper 编译、
三份源码树（`builddir`、`build-pkg-deb`、`build-pkg-rpm`）和两次大包压缩，不约束完整门禁。
`upload release assets` stage 用 `cnbcool/attachments:latest` 把 `dist/cefnest-build.tar.gz`、
四个 deb/rpm（两个包 × 两种格式）和 `SHA256SUMS` 放到该 tag 的 Release；上传失败会让
流水线失败，但产物仍留在该次构建的工作区里。

### 4.1 装包冒烟

发版过去只*生产*包：`Depends` 是手写的，没人证明过它们的名字存在；1.5 GB 的 CEF
运行时是不是真跟着 helper 落到了 `/usr/libexec/cefnest/<arch>/`，也没人在装完之后看过。
`scripts/ci-smoke-packages.sh` 补的就是这两段，按能在容器里做到的和做不到的分得很清楚：

- 六个上传物逐个点名，`sha256sum -c dist/SHA256SUMS` 必须通过，清单里必须有
  `cefnest-build.tar.gz`（清单只覆盖 `build-packages.sh` 自己产出的那些文件时，"全部产物
  的校验清单"这句话是假的）；`dist/` 里若还躺着 `libcefnest0`、`cefnest-helper`、
  `cefnest-demo` 或 `cefnest-packages-*` 这些 -6 的名称，直接判红；
- `dpkg -c` 逐个点名 `cefnest` 里的 helper 运行文件与那个带版本的 `.so`，并要求它**不**带
  demo 或 `shm-client`；`cefnest-dev` 必须带三个头文件与 `cefnest.pc`，且不带
  `libcef.so`；`chrome-sandbox` 的权限位必须是 `-rwxr-xr-x`（0755、非 setuid 是决定，
  写成判据才不会被以后某次"顺手修权限"改掉）；
- 两个包都必须把 machine-readable copyright 放在**数据层**的
  `/usr/share/doc/<包名>/copyright`，`cefnest` 那份还要有一段 `Files: usr/libexec/cefnest/*`
  的 BSD-3-clause 并带上 `LICENSE.cef.txt`。这一条是踩出来的：第一版写的是
  `DEBIAN/copyright`，看着像有了，其实 dpkg 根本不会安装控制层里的这个成员，`lintian`
  依旧报 `E: no-copyright-file`。判据直接从 `.deb` 的数据流里 `tar -xO` 那一个成员
  （一次 1.5 GB 的读流，十几秒），所以不装包也能量到；
- 以 root 安装本地的 `cefnest` 与 `cefnest-dev`：这一条是手写 `Depends` 唯一的真判据，
  名字写错、版本约束写错、依赖一个归档里不存在的包，都在这里当场失败。**但 apt 非零退出
  不等于 `Depends` 写错**——见 §4.2，第一次真跑就是这么判错的；
- 装完之后对 `/usr/lib/*/libcefnest.so.0` 与安装路径上的 `cefnest-helper` 跑 `ldd`，
  任何 `not found` 都算失败；
- 用安装后的 `cefnest.pc` 编译一个最小宿主，断言 `chromium_session_get_helper_path()`
  等于 `/usr/libexec/cefnest/<arch>/cefnest-helper`——这同时验证了 pkg-config、链接和
  编进库里的默认路径；**这条要 `cc`**，裸 `ubuntu:24.04` 没有，所以 job 脚本先装
  `gcc`；镜像里没有就记一条"没跑到"，不算包的失败；
- 解包 `cefnest-build.tar.gz`，确认 `src/libcefnest.so.<X.Y.Z>` 里记着的正是安装路径上的
  helper（这条路径编在库里，不在 demo 里），demo 的 `ldd` 在解包目录里全部满足；
  `test/shm-client` 也必须在那棵树里；**读那条路径要 `strings`**（属于 `binutils`），
  同上：job 里装，缺了就明说没测；
- 不带显示直接起一次 helper，读它的横幅：`helper: sandbox=on` 必须在（打包默认开沙箱），
  出现 `No usable sandbox!` 就是这次包在这个宿主上根本跑不起来。这条**不**能替你测
  Ubuntu 24.04 那套 AppArmor 限制（`apparmor_restrict_unprivileged_userns` 默认开）——容器复现不了别人机器上的策略，能复现的是"默认值
  到底是什么、以及响亮失败长什么样"（`docs/internals/sandbox-policy.md` §4 是量法）。
- RPM 侧只 `rpm -qlp`/`-qp`：冒烟 job 在 Debian 容器里，不会去装一个 RPM 数据库。

不在 root 时安装那一段会被跳过，而且会跳过得有声有色：不给 `CI_SMOKE_SKIP_INSTALL=1`
就直接退出失败，免得一条只做了 inspect 的绿线被读成"包装过了"。

### 4.2 第一次真跑（`v154.0.34-7`，2026-10-23 15:27）：三条红，只有第一条还没有答案

跑在 `image: ubuntu:24.04` 上，整个 stage `Finished, code: 1`。三条 `FAIL`：

1. `the install failed — a Depends string is wrong or unsatisfiable`，后面是 apt 那句
   `Errors were encountered while processing:` 列出的 17 个包——`tzdata`、
   `python3`/`python3.12`/`libpython3.12-stdlib`/`libpython3-stdlib`/`python3-packaging`、
   `libglib2.0-dev`/`libglib2.0-dev-bin`、`libpango1.0-dev`、`libgraphene-1.0-dev`、
   `libcairo2-dev`、`libgdk-pixbuf-2.0-dev`、`libharfbuzz-dev`、`libvulkan-dev`、
   `libgtk-4-dev`、`libjson-glib-dev`、`cefnest-dev`。
   **这句判词判错了对象**：紧跟其后的四条全过——装完之后
   `/usr/lib/*/libcefnest.so.0` 与 `/usr/libexec/cefnest/x86_64/cefnest-helper` 的
   `ldd` 都没有 `not found`、`/usr/bin/cefnest-demo` 确实没被装进来、
   `pkg-config --exists cefnest` 通过——也就是那 296 MB 的主包和 dev 包是真落到盘上、
   loader 也是自洽的；而 `Depends` 里有一个名字写错，apt 连解析都过不去，根本走不到
   "配置这一堆发行版包"这一步。所以这一次失败**为什么**失败，在转录里读不出来：脚本只
   `tail -20`，而 apt 的尾巴恰好是不含原因的那 20 行——谁先坏、坏在哪一步在更前面。
2. `the pkg-config compile failed`，同一行 `cc: not found`：裸镜像里没有编译器。
3. `the build-tree library does not name /usr/libexec/cefnest/x86_64/cefnest-helper`，
   同一行 `strings: not found`：裸镜像里没有 binutils。

2 和 3 改掉的理由是同一条：**缺工具是关于镜像的事实，不是关于包的证据**，所以按 `rpm`
那条已有的口径降级成 `..  没跑到` 的 note；同时 job 的脚本先 `apt-get install -y
--no-install-recommends gcc binutils`，把这两条在 CI 上真正的覆盖拿回来（装不上就整条
stage 红，而不是悄悄少测两项）。1 只改 reporting：失败时依次打第一条
`dpkg: error` / `E:` / `No space left on device`、我们自己两个包的 `dpkg -l` 状态、
`df -h /usr /var`，最后才打那段尾巴。

1 到底是什么，现在只能说"和这两个包的文件无关、发生在这个事务里"。该排的顺序：
**容器磁盘**（这个 stage 前面是一个 90 分钟的 build stage，把 ~1 GB 的 CEF 下载、铺成
296 MB 的 deb，装的时候还要在 `/usr` 落 1.5 GB 以上；而 dev 包那串 `-dev` 依赖把 GTK
全家拖进同一事务，是这次比历史上任何一次都大的原因）、`tzdata` 那个绑定挂载的
`/etc/localtime`（`ci-build.sh` 为它做过绕行，本脚本也 umount 一次，但容器里的 `umount`
不保证有权）、以及 GTK 依赖链上某个 postinst 在这台 runner 上真的坏。**下一次跑那三行
会直接给答案，答案出来之前不动 `Depends`。**

同一次跑里另外两条与"包能不能用"相关的证据是好的：`..  unprivileged user namespaces in
this container: no`、`helper: sandbox=on`、`no sandbox FATAL on this host`——容器不是
AppArmor 那种宿主（`docs/internals/sandbox-policy.md` §4），但打包默认值在这台上是"开"
且没有当场炸。

## 5. 发版（tag push）

普通分支推送不触发任何构建。发版推一个与包版本-修订一致的 tag：

```bash
git tag v154.0.34-7
git push origin v154.0.34-7
```

`.cnb.yml` 的 `$: tag_push` 按 §4 那些 stage 依次跑：`scripts/check-release-tag.sh`
先比版本串与 tag，`scripts/ci-build.sh` 才做重活（导出 `CEFNEST_PACKAGE_RELEASE=7`，
构建时生成中英双语的 `RELEASE-NOTES.md`，含“本版变更”清单与附件说明表格），
`scripts/ci-smoke-packages.sh` 把 `dist/` 里的包装一遍，然后：

- `git:release` 用该 tag 创建 Release，描述从 `RELEASE-NOTES.md` 读取；
- `cnbcool/attachments` 把 `dist/` 里那 6 个文件上传到该 tag 的 Release。

顺序不能反：附件插件要求 Release 已经存在。重发同版本时要递增修订号，而 `.cnb.yml`
里跟着动的有 **8 个位置**：三行 `CEFNEST_PACKAGE_RELEASE=7`（check、build、smoke
各一处）、一行 `CEFNEST_EXPECTED_TAG=v154.0.34-7`、以及 4 个带版本的静态附件名——
`cefnest-build.tar.gz` 与 `SHA256SUMS` 这两个名字永远不带版本。附件列表不展开 shell
变量，所以这份清单只能手写；上面第一个 stage 就是为它手写清单兜底的，漏改哪一处都在
几十秒内失败，而不是 90 分钟后少一个文件。tag 名要与包版本一致（`v154.0.34-7`），
包版本本身仍由 CEF 的 `X.Y.Z` 决定。

`create release` 带着 `latest: true`，而 CNB 没有"只有最新的 tag 才标 latest"这种条件：
重推一个旧 tag 会让旧版本抢走 latest 标记。发版顺序保持向前即可避开；要回退标记只能
在页面上改。
