# CEF 运行时裁剪:改动与落地方案(2026-10-25)

> **修订(2026-10-25,复核)**:对 pin 的 154 独立复测——`libcef.so` 副本
> strip 后 272 219 288 B、`nm -D` 的 `cef_*` 导出 strip 前后均 243,与本文
> 数字一致;两处方案修正:strip 的落点从 `ci-build.sh` 改到
> `build-packages.sh`(见第 3 节),chrome-sandbox 的 4755 说法撤回(见
> 第 5 节)。

`cefnest` 包把 helper 的 CEF 运行时一起发出去,安装后约 1.5 GB,这是本仓库最重的
一块。本文记录一轮实测:只做 **strip** 和 **删除 dev 文件**(locale 按最终决定
**全量保留**),在独立的 156 发行包上量出收益,再用 CEFNest 自己跑一遍确认
可用,最后给出落到 `ci-build.sh` / `build-packages.sh` 的方案。

结论:1.49 GiB 的运行时目录可以降到约 347 MiB(154 实测推算),功能不受影响;
**strip 只减磁盘,不减内存**,内存优化要另做,不在本文范围。

## 结论速览

| 项 | 数值 |
|---|---|
| strip 前 `libcef.so`(156) | 1 476 332 936 B = 1407.9 MiB |
| strip 后 `libcef.so`(156) | 276 884 632 B = **264.1 MiB**(-81%) |
| strip 前 `libcef.so`(154,pin 的版本) | 1 455 021 248 B = 1387.6 MiB |
| strip 后 `libcef.so`(154) | 272 219 288 B = **259.6 MiB** |
| 154 运行时目录(strip 前) | 1487.2 MiB |
| 154 运行时目录(仅 strip `libcef.so`,推算) | **359.2 MiB** |
| 再 strip `libvk_swiftshader.so` / `libvulkan.so.1` / helper(实测) | -11.8 MiB → **约 347 MiB** |
| 156 独立裁剪包(全 locale,只 strip 了 `libcef.so`) | 371 MiB;`tar.zst` **119 MB** |
| 导出符号 | strip 前后均为 **243** 个 `cef_*`,`dlopen` 通过 |
| 单进程内存 | strip 前后 PSS 基本不变(382 vs 386 MiB) |

## 1. 本轮改动(独立 156 发行包)

产物在本机工作区,不属于本仓库,不进 git:

- `cef_156_linux64_trimmed/` —— 371 MiB,14+ 个运行时文件
- `cef_156_linux64_trimmed.tar.zst` —— 119 MB(官方 `tar.bz2` 为 316.4 MiB)

做的三件事:

1. `strip --strip-unneeded Release/libcef.so`:官方 `minimal` 包不做 Linux strip
   (`tools/make_distrib.py` 只处理 Windows PDB 与 macOS dSYM),而
   `is_official_build=true` 会把 `symbol_level` 强制为 2,DWARF 全部内嵌在
   `.so` 里,这就是 1.4 GB 的来源。
2. 删除 dev 文件:`include/`、`libcef_dll/`、`cmake/`、`bazel/`、`WORKSPACE`、
   `BUILD.bazel`、`MODULE.bazel`、`.bazelrc`、`.bazelversion`(合计约 5.5 MiB)。
   这些是**构建期**需要、运行期不需要的东西,所以只能删运行树,不能删
   `third_party/cef/dist`。
3. locale **全量保留**(决策见第 4 节)。最初试过只留 `en-US`,随后按"保留完整
   语言"改回:220 个文件与上游逐项 diff 一致,共 48.9 MiB。

没动的:SwiftShader/Vulkan(15.8 MiB)、`resources.pak`、`icudtl.dat`、
`v8_context_snapshot.bin`、`chrome-sandbox`、`LICENSE.txt`/`CREDITS.html`。

本文档的 156 独立包只 strip 了 `libcef.so`;其余三个二进制(SwiftShader、Vulkan
壳、helper)的 strip 写在打包脚本里(第 3 节),所以上面的 371 MiB 不含那
11.8 MiB 的收益。

**一个容易误判的细节**:220 个 locale 文件里 55 个是真实语言 pak,另 165 个是
`xx_FEMININE.pak` / `xx_MASCULINE.pak` / `xx_NEUTER.pak`,每个只有 18 字节
(空 pak 头)。它们来自上游构建,不是本地污染;将来按需裁语言时,真正的收益
来自那 55 个文件。

## 2. 怎么验证的:用 CEFNest 自己跑

共享仓库只验证、不改不推:全新浅克隆到 `/tmp/cefneset_fresh`,构建全部在树外
(`/tmp/cefneset_build`),`git status` 保持干净。

- `cmake -S helper -B /tmp/cefneset_build/helper -DCEF_ROOT=<156 dist>`,编译
  `libcef_dll_wrapper` + `cefnest-helper` 通过;
- `meson setup /tmp/cefneset_build/meson .` + `ninja`,87 个目标全过;
- `cefnest-demo` 用 `CHROMIUM_DUMP` 渲染 7 s 后自动退出,截图核对内容。

两次运行的结果不同,原因值得记住:

| 运行时布局 | 结果 |
|---|---|
| 官方布局:资源在 `Resources/` 子目录 | **崩溃**:`Invalid file descriptor to ICU data received` → SIGTRAP(5) |
| 扁平布局:资源与 helper 同目录(`libexec/cefnest/<arch>/`) | **通过**:`sandbox=on`,2000×1218 正常渲染,正常退出 |

崩溃与 strip 无关:helper 没有设 `resources_dir_path`,按 CEF 默认要求资源在
exe 所在目录。仓库打包走 `install_subdir(helper_build_dir)`,本来就是把 cmake
输出目录(helper + `libcef.so` + 资源)整体拷进 `libexec/cefnest/<arch>/`,所以
**打包形态天然是扁平的**;只有手工拼包时才会踩到。

功能面:页面正常渲染;右键菜单显示"返回/前进/打印/查看网页源代码",说明中文
locale 正常加载。内存面:同一页面下总计 PSS 382.3 MiB / 10 进程,全 locale 版
385.9 MiB / 11 进程 —— **strip 不影响内存**(调试段本来就不映射)。

## 3. 落地方案

改动只有一处,加在 `scripts/build-packages.sh`:版本检查之后、deb 树
`ninja install` 之前。`ci-build.sh` 把打包整体委托给 `build-packages.sh`
(它以 `CEFNEST_EXTRA_ARTIFACTS=... ./scripts/build-packages.sh` 收尾),所以
strip 放进 `ci-build.sh` 只能覆盖 CI 路径——本地直跑 `./scripts/build-packages.sh`
打出来的仍是 1.5 GB 的包;而 `build-packages.sh` 自己配置两套 meson 树
(deb 的 multiarch libdir 与 rpm 的 lib64)并各自 `DESTDIR=… ninja install`,
`install_subdir` 是在 install 阶段才读源码树的 `helper/build/Release`
(`meson.options` 的 `helper-build-dir`,默认 `helper/build`),在脚本入口处
strip 一次就同时覆盖 CI 与本地、deb 与 rpm:

```sh
# scripts/build-packages.sh,版本检查之后、meson setup 之前
REL="$ROOT/helper/build/Release"
for f in libcef.so libvk_swiftshader.so libvulkan.so.1 cefnest-helper; do
  [ -f "$REL/$f" ] || continue
  strip --strip-unneeded "$REL/$f"
done
```

顺序约束:strip 必须发生在两套包树 `cp -al` 硬链接之前(上述位置天然满足)。
`cp -al` 之后 deb 树与 rpm 树、以及源目录共享同一批 inode,届时再去 strip
"某一侧的副本"会同时改写两边的包内容。

四个文件都只丢 `.symtab` / `.debug_*`,动态符号表与 Vulkan ICD 导出不变,SwANGLE
仍可加载;`cefnest-helper` 会被 strip 是因为 cmake 的 Release 带 `-g`(实测
1.4 → 0.9 MiB)。

- 校验(建议进 gate,都很廉价):
  `nm -D --defined-only "$REL/libcef.so" | grep -c ' cef_'` 应为 243;
  `for f in libcef.so libvk_swiftshader.so libvulkan.so.1 cefnest-helper; do
  readelf -S "$REL/$f" | grep -c '\.debug_' || true; done` 应全为 0。
- **不要在 CI 里存未 strip 副本**:需要符号时按 `third_party/cef/dist/.cef-version`
  重跑 `scripts/fetch-cef.sh` 就能拿回同一份库(有 SHA256 钉,
  `PINNED_SHA256`),比在 runner 上
  留 1.4 GB 更省。尤其不要放进 `artifact/`:`ci-build.sh` 用
  `tar -czf cefnest-build.tar.gz -C artifact .` 打包整个目录,副本会直接进发布物。
- `strip` 不改文件权限位,helper 仍可执行。
- `libcefnest.so` 可选:包用 `--buildtype=release`,没有 `.debug_*`,只省几十 KB
  的 `.symtab`。
- **不要**在 `third_party/cef/dist` 上删 `include/`/`libcef_dll/`:构建 helper 需要;
  deb 也不会带上它们。
- 本地 `meson install -Dinstall-helper=true`(不经 build-packages.sh)不做 strip,
  开发树保持原样——符号化要用;装机包一律走打包脚本。
- locale 全量保留 → 打包脚本无需改。若将来要裁,裁的是
  `helper/build/Release/locales`(55 个真实 pak),仍不是 dist。
- `cefnest-build.tar.gz` 不含 helper 与 CEF 运行时,不受影响。

效果(按 154 实测推算):`cefnest` deb 安装后 1487.2 → 359.2(仅 strip `libcef.so`)
→ **约 347.4 MiB**(四件全 strip);压缩后量级参照 156 实测(367 MiB → 119 MB)。
上传/下载与 runner 的 `cp -al` 1.5 GB 硬链接也一并变轻。

## 4. 决策记录

- **locale 全量保留**。仅 `en-US` 可再省 48 MiB,但 Chromium 原生 UI(右键菜单、
  权限弹窗、错误页)会退回英文;而 `en-US + zh-CN` 只需多 0.57 MiB。既然最终
  选了全量,就按全量打包,不做语言裁剪。
- **不删 SwiftShader**(15.8 MiB):保留软件 GL 回退,避免无 GPU/驱动异常时白屏。
  但会 strip 它(14.3 → 3.8 MiB)和 Vulkan 壳(1.5 → 0.7 MiB),只去调试段,
  SwANGLE 功能不变。
- **不做 `--no-symbols` 之类的期待**:它对 Linux 无效,只影响 PDB/dSYM。

## 5. 风险与未验证

- 本轮端到端验证用的是 **156**,仓库 pin 的是 **154**(带 SHA256 钉)。154 只做了
  strip 后 `dlopen` + 导出符号检查,未跑完整 demo;154→156 的里程碑升级还需要
  按仓库约定重跑完整 gate(protocol 上限、session 脚本)。
- strip 后本地符号化失效:崩溃栈只有地址。需要符号时按 `.cef-version` 重跑
  `scripts/fetch-cef.sh` 取回未 strip 的库(有 SHA256 钉),线上用符号服务器解析。
- ~~chrome-sandbox 系统安装时必须 `root:root` + `4755`,否则沙箱起不来~~
  **(修正,2026-10-25 复核)**:本仓库不是这样。`build-packages.sh` 的包描述
  明写"chrome-sandbox is installed 0755: this package installs no setuid
  root helper"——CEFNest 依赖的是 Chromium 基于**非特权用户命名空间**的
  layer3 沙箱,不是 SUID helper;cmake 构建输出的 chrome-sandbox 本来就是
  `0755`。被 userns 限制的系统(Ubuntu 24.04+ 的
  `apparmor_restrict_unprivileged_userns=1`)按 README 常见问题处理:给
  helper 配允许 userns 的 AppArmor profile,或 `CHROMIUM_HELPER_NO_SANDBOX=1`。
  给 chrome-sandbox 补 4755 在这里既无必要也和包的声明相悖。
- 内存没有变化,不要把这轮裁剪当成内存优化。

## 6. 复现

```sh
# 1. 取 156 minimal 包,只解 libcef.so 并 strip
tar -xjf cef_binary_156.0.14+*_linux64_minimal.tar.bz2 -C /tmp/x --wildcards '*/Release/libcef.so'
strip --strip-unneeded /tmp/x/cef_binary_*/Release/libcef.so

# 其余三件(在组装好的运行目录里,实测 14.3→3.8 / 1.5→0.7 / 1.4→0.9 MiB)
strip --strip-unneeded /tmp/rt/libvk_swiftshader.so /tmp/rt/libvulkan.so.1 /tmp/rt/cefnest-helper

# 2. 组装运行树(扁平)并跑 CEFNest demo
CHROMIUM_URL='data:text/html,<h1>hi</h1>' \
CEFNEST_HELPER=/tmp/rt/cefnest-helper \
CHROMIUM_DUMP=/tmp/shot.png CHROMIUM_DUMP_DELAY=7000 \
./builddir/src/cefnest-demo
```
