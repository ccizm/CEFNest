# 沙箱：这个组件的决定，以及三条路径的实测

日期：2026-10-20
问题：任务 #22。helper 自打进仓库（ce72088）就写着 `settings.no_sandbox = true`，旁边补一句
"待决定"，`OnBeforeCommandLineProcessing()` 里还额外 append 了一个 `no-sandbox` 开关，
注释说 CefSettings 只管浏览器进程、zygote 和 renderer 得自己被告知。README 的
Known limitations 里对应一条"沙箱策略未定，请把这个进程当作无沙箱进程"。
这一篇把"未定"关掉。

## 1. 决定

**默认开沙箱。关掉它的唯一办法是显式说 `CHROMIUM_HELPER_NO_SANDBOX=1`。没有任何静默回退。**

代码上就是三处（`helper/src/main.cpp`）：

```cpp
bool SandboxDisabled() {                                    // 唯一的读者
  return EnvOrEmpty("CHROMIUM_HELPER_NO_SANDBOX") == "1";
}
...
settings.no_sandbox = SandboxDisabled();   // 不设环境变量时是 false
...
fprintf(stderr, "... helper: sandbox=%s\n", ...);   // 环境横幅里报出来这次是哪一种
```

`"1"` 是唯一算数的值：一个安全 knob 上的拼写错误应该让这次运行失败，而不是让沙箱消失。

## 2. 为什么在这条路上"开"是免费的：那个 setuid helper 根本没用上

CEF 154 的 `Release/chrome-sandbox` 权限是 `-rwxr-xr-x`——**不是 setuid**。这很容易让人
以为"要真开沙箱就得 root 装一下"，于是先关掉再说。实测不是这样：把 `chrome-sandbox` 整个
从 `Release/` 里移走，再跑第 5 节那份 `test/wheel.session`，加载、JS 往返、三个滚轮数全部
一字不差——`HTTP 200`，然后 `360,360`、`420,360|60`、`1020,360|60|600`。

本机给的是**非特权用户命名空间**：

```
/proc/sys/kernel/unprivileged_userns_clone = 1
/proc/sys/user/max_user_namespaces        = 157973
kernel.apparmor_restrict_unprivileged_userns  不存在（没装那套限制）
```

这台宿主上走的确实是命名空间沙箱：`chrome-sandbox` 不在场，没有任何一处行为变化。那个文件是
给"没有 userns"的宿主准备的老路（SUID sandbox）。

沙箱开着的进程树（`test/wheel.session` 那一跑，`Seccomp` / `NoNewPrivs` 取自 `/proc/PID/status`，
命名空间取自 `/proc/PID/ns/`；去掉 `cefnest-demo` 自己，helper 一家 9 个进程）：

| pid | ppid | Seccomp | NNP | CapEff | user ns | pid ns |
| --- | --- | --- | --- | --- | --- | --- |
| 130008 | 129999 | 0 | 0 | `…0000` | 4026531837 | 4026531836 |
| 130025 | 130008 | 0 | 1 | `…0000` | 4026531837 | 4026531836 |
| 130026 | 130008 | 0 | 1 | `…0000` | **4026533176** | **4026533119** |
| 130028 | 130026 | 0 | 1 | `…200000` | 4026533176 | 4026533119 |
| 130060 | 130025 | 0 | 1 | `…0000` | 4026531837 | 4026531836 |
| 130062 | 130008 | 0 | 1 | `…0000` | 4026531837 | 4026531836 |
| 130089 | 130028 | **2** | 1 | `…0000` | 4026533176 | **4026533116** |
| 130100 | 130028 | **2** | 1 | `…0000` | 4026533176 | **4026533117** |
| 130101 | 130028 | **2** | 1 | `…0000` | 4026533176 | **4026533177** |

130008 是浏览器进程（不沙箱，本来就不该沙箱——Wayland 连接、GDK、dmabuf fd、控制通道 socket
都在它手里）。130026/130028 是进了自己 user ns 的 zygote，`Seccomp` 还是 0：**seccomp 过滤器
出现在 zygote 再 fork 出来的那一层**，也就是 130089 / 130100 / 130101——`Seccomp=2`、`CapEff=0`、
`NoNewPrivs=1`，而且各自一个 pid ns。剩下 130060、130062 两个进程和 zygote 一样停在
`Seccomp=0`。

**叶子的类型标签读得出来，但要按空格切。** Chromium 给子进程重写了 argv：`/proc/PID/cmdline`
只剩 **1 项**，但整条命令行——`--type=`、`--utility-sub-type=` 都在内——就挤在那一个
`argv[0]` 字符串里，按 NUL 切分什么都得不到，按**空格**切那一项就全回来了
（`docs/features/audio-path.md` 第 1 节那张带类型的表就是这么取的）。`/proc/PID/comm` 倒是真的
全都是 `cefnest-helper`。后来补的一张带类型分布的表：浏览器进程、三个 zygote（gpu 的
父进程带 `--no-zygote-sandbox`）、`gpu-process`、`network.mojom.NetworkService` 全是
`Seccomp=0`；`storage.mojom.StorageService` 和两个 renderer 是 `Seccomp=2`；音频则住在一个
**不沙箱**的 `audio.mojom.AudioService` utility 里。

## 3. 关沙箱只需 CefSettings，那个 `--no-sandbox` 开关是多余的

`settings.no_sandbox = true` 单独一条就够到整棵进程树。只设它、不在
`OnBeforeCommandLineProcessing()` 里 append 任何东西，跑 hosted 模式，7 个子进程
`/proc` 里全是 `Seccomp=0`（同一台机器、同一个 session 脚本、和第 2 节同一套取数办法）：

| pid | ppid | Seccomp | NNP | user ns | pid ns |
| --- | --- | --- | --- | --- | --- |
| 129866 | 129857 | 0 | 0 | 4026531837 | 4026531836 |
| 129882 | 129866 | 0 | 1 | 4026531837 | 4026531836 |
| 129883 | 129866 | 0 | 1 | 4026531837 | 4026531836 |
| 129912 | 129882 | 0 | 1 | 4026531837 | 4026531836 |
| 129915 | 129866 | 0 | 1 | 4026531837 | 4026531836 |
| 129941 | 129883 | 0 | 1 | 4026531837 | 4026531836 |
| 129954 | 129883 | 0 | 1 | 4026531837 | 4026531836 |
| 129956 | 129883 | 0 | 1 | 4026531837 | 4026531836 |

所以那个 append 删了：`CefSettings` 一条就够到整棵进程树，机制没有再往里查（CEF 是不是把
开关写进浏览器命令行、子进程又怎么继承的，本篇只承认"量到七个子进程全是 `Seccomp=0`"这一件
事）。原来注释里的判断（"子进程会自己去建沙箱，必须告诉它们"）不成立；两处各写一遍只会给它们
漂移的机会。顺带记一个观察：关了沙箱之后 `NoNewPrivs` 仍然是 1，而我们没有设过它——那是
CEF/Chromium 自己降权时设的，不属于这次决定的范围，原因没在这里查。

## 4. 没有沙箱可用时，失败是响的（造出来的，不是推的）

`bwrap --unshare-user --disable-userns` 能把"嵌套用户命名空间被禁"这件事搬到本机来测。
里面 `/proc/sys/user/max_user_namespaces` 读出来是 `2147483647`，
`/proc/sys/kernel/unprivileged_userns_clone` 还是 `1`，可是 `unshare(CLONE_NEWUSER)` 直接
返回 `ENOSPC`——看文件是看不出来这类限制的。

先跑独立模式的 helper（`cefnest-helper https://example.com/`，它拿到的是宿主的 `wayland-0`，
不过在建立窗口之前就死了，宿主桌面上没有多出任何东西）：

```
helper: sandbox=on
[129754:129754:FATAL:content/browser/zygote_host/zygote_host_impl_linux.cc:129] No usable sandbox!
    If you are running on Ubuntu 23.10+ … see apparmor-userns-restrictions.md …
    If you want to live dangerously … you can try using --no-sandbox.
Trace/breakpoint trap
```

hosted 侧看到的是（同一个 bwrap，跑 demo + helper；嵌套合成器自己照常起来了，
`nested compositor on WAYLAND_DISPLAY=wayland-1` 那行在）：

```
session: helper stopped (control channel closed)
session: helper killed by signal 5
```

同一份 bwrap 里加 `CHROMIUM_HELPER_NO_SANDBOX=1`，页面就起来了：

```
helper: sandbox=OFF (CHROMIUM_HELPER_NO_SANDBOX=1)
demo: loaded https://example.com/ (HTTP 200)
```

两半都成立：**响亮失败 + 显式 opt-out**。唯一要补的是那句 FATAL 教用户用的是
`--no-sandbox`，不是我们的 knob——所以横幅里现在有 `helper: sandbox=…` 这一行，
读转录的人一眼能看到这次跑的是哪一种。

上面这种是数量上限那一类（`ENOSPC`）。AppArmor 那一类限制报什么 errno、是不是停在同一句
`No usable sandbox!` 上，没试过——那句 FATAL 里专门提到了 Ubuntu 23.10+（那个版本引入了这个
sysctl，但**默认是 0**；默认开到 1 的是 Ubuntu 24.04），看语气是同一条出口，但这一条留给
需要它的人去量。没去动宿主自己的 sysctl。

## 5. 沙箱不改变任何数字

同一份 `test/wheel.session`（`hover` 到页面上，`wheel 0 3` → `wheel 0 0.5` → `wheel 0 5`，
每步读 `document.scrollingElement.scrollTop` 和 DOM 里收到的 `deltaY`），三种配置：

| 配置 | 加载 | JS 往返 | `wheel 0 3` | +半格 | `wheel 0 5` |
| --- | --- | --- | --- | --- | --- |
| 默认（沙箱开 + GPU） | HTTP 200 | 通 | `360,360` | `420,360\|60` | `1020,360\|60\|600` |
| 沙箱开 + `CHROMIUM_HELPER_DISABLE_GPU=1` | HTTP 200 | 通 | `360,360` | `420,360\|60` | `1020,360\|60\|600` |
| `CHROMIUM_HELPER_NO_SANDBOX=1` | HTTP 200 | 通 | `360,360` | `420,360\|60` | `1020,360\|60\|600` |

三行逐字相同。每个单元格里逗号前那个数是 `scrollTop`，逗号后是 DOM 里收到的 `deltaY` 序列。

`js` 那一条值得单独说：`window.__report` 是 **renderer 发 `CefProcessMessage` 给浏览器进程**、
再由浏览器进程写进控制通道的。这条 IPC 就是剪贴板、文本输入、`ConsoleOnMessage` 一类页面侧功能
所依赖的路径——它在 `Seccomp=2` 的 renderer 上跑通了。反过来，剪贴板和 text-input 的 Wayland
那一端都在浏览器进程里（`nested-data-device.c` / `nested-text-input.c` 是宿主控件那侧的），
沙箱根本碰不到它们。所以这两块没有另做端到端复测：该复测的是跨进程那条线，而它已经绿了。

## 6. 部署上要记住的事

* **不需要 setuid 安装步骤。** 目标宿主只要有非特权用户命名空间，什么都不用装。deb 和 rpm
  都把 `chrome-sandbox` 原样装成 0755（`helper/` 里 CEF 分发就是 0755，打包不动它），装包
  不产生任何提权面；`scripts/ci-smoke-packages.sh` 每次发版把这个权限位当判据断言一次，
  免得哪天"顺手修权限"把它改成 setuid。
* 宿主禁了 userns 时，helper 会在启动那一步响亮地死掉（第 4 节量到的 `No usable sandbox!`），
  **不会**退回无沙箱运行。这一类限制现在最常见的来源是 Ubuntu 24.04：
  `apparmor_restrict_unprivileged_userns` 在 23.10 就有，但默认开着的是 24.04。两条被认可的出路：
  1. **给这个应用单独加 AppArmor profile**（推荐，粒度是一个应用，页面代码继续在沙箱里）。
     Ubuntu 给 Google Chrome 用的就是这一形；把 helper 的**安装路径**填进去：
     ```
     abi <abi/4.0>,
     include <tunables/global>

     profile cefnest-helper /usr/libexec/cefnest/x86_64/cefnest-helper flags=(unconfined) {
       userns,
       include if exists <local/cefnest-helper>
     }
     ```
     存成 `/etc/apparmor.d/cefnest-helper`，`sudo apparmor_parser -r` 或 `sudo systemctl reload apparmor`。
     这一形是按 Ubuntu 发行说明写的，**本仓库没在开了限制的 Ubuntu 上量过**（本机 Debian 13
     根本没有这个 sysctl，第 2 节）；装的是 rpm 的话路径在 `/usr/libexec/cefnest/x86_64/` 同一处。
  2. **`CHROMIUM_HELPER_NO_SANDBOX=1`**：明确放弃沙箱，页面代码从此以宿主用户身份跑，横幅里留痕。
* 那句 FATAL 自己还给第三条路——把 `chrome-sandbox` 装成 `chmod 4755`（老的 SUID sandbox）。
  **本项目不走这条，也不建议**：它要 root 改一个 CEF 分发里的文件、给整机立一个 setuid 靶子，
  而第 9 节记着这条在这棵树上根本没量过——"命名空间不可用时它确实能顶上"只是 Chromium 文档的说法。
* **不要**为了这个组件去关整机的 `apparmor_restrict_unprivileged_userns`。
* 沙箱保护的是页面代码所在的 renderer，**不是** helper 整体。浏览器进程永远不沙箱，
  因为它握着 Wayland 连接和控制通道；把"helper 进程"当成一个可信边界是错的，
  正确的边界是"renderer 里的东西都不可信"。
* `CHROMIUM_HELPER_NO_SANDBOX=1` 在启动横幅里留了痕（`sandbox=OFF (…)`），转录里能看到。

## 7. 长跑：48 秒、28 次导航、两次把 renderer 打死（2026-10-21 补）

第 4 节那次只 `kill -9` 了一回，而且量的是"关沙箱 vs 开沙箱"的数字差。这一节量的是**开沙箱、
一直开着、反复导航、中途两次打死 renderer** 时这棵树本身的形状。先是一次性测量，跑法：一次会话，
28 轮 `load` 在四个本地页（`dnd-page` / `find-page` / `wheel-page` / `menu-page`）之间轮流，每 4 轮插一次
`reload-hard`，每轮后面跟一次 `js document.title` 读回；在 t=18 s 和 t=40 s 各挑一次
`--type=renderer` 的子进程 `kill -9`。每 2 秒采一行：

| t (s) | 进程数 | `--type=` 子进程 | renderer | 僵尸 | fd 合计 | 树 RSS | 挂着 crashpad 的 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | 9 | 8 | 2 | 0 | 364 | 883 MB | 0 |
| 18 | 9 | 8 | 2 | 0 | 371 | 903 MB | 0 |
| 20 | 9 | 8 | 2 | 0 | 370 | 893 MB | 0 |
| 40 | 9 | 8 | 2 | 0 | 374 | 906 MB | 0 |
| 42 | 9 | 8 | 2 | 0 | 372 | 894 MB | 0 |
| 48 | 9 | 8 | 2 | 0 | 378 | 897 MB | 0 |

四件事：

* **树不变形**：进程数 9、`--type=` 的 8、renderer 2 从第一次采样到最后一次，一次没变。
  两次击杀之后下一次采样就又回到 2 个 renderer——zygote 还在，补出来的 renderer 是它的孩子，
  不是浏览器进程的孩子。
* **fd 只涨了 14 个**（364→378），而这段里有 7 次硬重载；RSS 是锯齿不是斜坡（903→906 之后
  回落到 894→897），28 次导航 + 28 次 JS 读回全部有应答。没有单调增长可报。
* **两次击杀 = 两条事件**：`demo: renderer exited status=1 error=9 9` 正好两行，一行一次，
  不多报也不漏报（`status=1` 是 `STATUS_CRASHED`，`error=9` 是信号号，见第 8 节）。
* **退出干净**：demo 走完之后 `pgrep -f cefnest-helper` 剩 0 个——没有孤儿 zygote，没有滞留的
  utility 进程。

采样本身踩到一个坑，值得记下来，因为任何"给这棵树做监控"的人都会撞到：**从浏览器进程往下数看不到
renderer**。renderer 是 zygote 的孩子，不是浏览器进程的孩子，所以 `pgrep -P <浏览器 pid>` 只给
`zygote` / `gpu-process` / `network utility` 三个，打死一个 renderer 之后那个数字纹丝不动——
第一版量出来的"children 恒等于 3"就是这么来的。要看全树得按**可执行文件**横切：命令行里跑的是
`cefnest-helper` 的那个二进制（含 `--type=` 的和不含的都算），再各自读 `/proc/PID/cmdline` 切类型。
`docs/features/audio-path.md` 那张进程表是同一件事的另一半：类型标签只在 cmdline 里，`comm` 给不出来。

### 已经收进 `test/sandbox-soak.sh`

一次性的量法第二天就搬进了仓库：`./test/sandbox-soak.sh`，20 轮导航、t=12 s 与 t=28 s 各一次击杀、
每 2 秒采样，断言的是树的形状而不是像素——它会开一个真窗口约 40 秒，所以按本项目的约定是一个
**人工门**（和 `test/find-gate.sh`、`test/dnd-gate.sh` 同一类），不进 `meson test`。判据八组：样本够多
且两次击杀都真找到过 renderer；全程零僵尸；全树任何一格都没出现过 `--crashpad-handler-pid`（第 8 节
的结论要是哪天变了，这一格先响）；被杀那次的父 pid 不等于浏览器进程 pid（就是上面那个坑的反向断言）；
从 t≥16 起进程数与 renderer 数一格不变；fd 的增量和峰值都压在 `FD_SLACK`（默认 40）以内——RSS 只报不卡，
它是锯齿而且是机器相关的；`ROUNDS-2` 次以上的 JS 读回有应答；恰好两条 `renderer exited status=1 error=9`；
demo 退出后 helper 剩 0 个。

第一次跑（2026-10-21，本篇表格之外的数都来自这一趟）：40 秒、19 个样本，9 个进程 / 8 个带 `--type=` /
2 个 renderer 从 t=16 到最后一次采样没动过，fd 379→378、峰值 386，最后一格 RSS 898 MB，20 次读回 18 次
有应答，两条死亡事件，0 残留，12 行 ok 全绿。读回为什么写成下限：一次 `kill -9` 正好落在某轮 `js`
发出去的那一毫秒时，那一次读回必然丢，所以「20 次全应答」不是判据而是运气；上面那次量到 28/28 是同一
棵树在另一组击杀时刻上的结果。基线取 t≥16 而不是第一格，同理：第一格常在页面进程起来之前，拿它当
"泄漏前的基准"会把启动本身算成增长。

## 8. crashpad：这里没有要修的，也没有能关的（2026-10-21 补）

任务清单上这一条写的是"crashpad 修复或显式关闭"，前提是有个崩溃报告器在跑或者该跑。实测两个都不成立。

我们这棵树里**没有任何崩溃处理器**：

* 每个子进程的命令行带的都是 `--enable-crash-reporter=,` ——逗号后面是空的，没有 reporter id，
  也没有产品短名；
* 全树没有一处出现 `--crashpad-handler-pid=`，而这一项正是"孩子挂在一个真实 handler 上"的可见标志。
  同一台机器上那两个 Electron 应用的孩子每一行都有它，而且它们各有一个活的
  `chrome_crashpad_handler … --database=/home/siem/.config/…` 进程做父兄；
* CEF **minimal** 分发根本不装那个可执行文件。安装后的 helper 旁边一共这么多东西：
  `cefnest-helper`、`chrome-sandbox`、`chrome_100_percent.pak`、`chrome_200_percent.pak`、
  `extensions`、`icudtl.dat`、`libcef.so`、`libvk_swiftshader.so`、`libvulkan.so.1`、
  `locales/`（220 个文件）、`resources.pak`、`v8_context_snapshot.bin`——没有 `crashpad_handler`。

所以"修复"没有对象（要修得先往分发里塞一个仓库里没有的可执行文件，再给它一个上报端点和一个产品标识，
那是一件需要产品决定的事，不是一个 bug）；"显式关闭"也没有开关可拧：那个空值不启动任何进程，
手动补一条 `--disable-crash-reporter` 不会改变第 7 节表里的任何一格，所以不加。

真正要写进部署文档的是**它的后果**：一次崩溃能给到宿主的只有 OS 那层的死法。`::renderer-exited`
给的是 `status=1 error=9`——CEF 的终止状态加一个信号号，没有 minidump，没有栈，没有崩溃时的
URL。第 7 节那两次击杀量的就是这个上限：宿主能确定"死了几个、怎么死的"，不能确定"为什么"。
`chrome-sandbox` 随安装落到 prefix 里时权限没变（`mode=0o755`、uid 还是装机的人、
`security.capability` 无），装这个包本身不产生任何提权面——第 6 节那条"不需要 setuid 安装步骤"
在打包之后依然成立。

## 9. 还没做的

* **没做运行时预检**（"先探一探沙箱起不起来，再决定怎么报错"）。不打算做，两个理由：
  第 4 节量到 sysctl 读起来全绿而 `unshare` 仍然失败，所以"读 `/proc` 判断这台宿主能不能
  沙箱"这种预检本来就会给出错误答案，真要预检只能 fork 一个子进程去 `unshare` 一次——而
  `Control::Connect()` 已经起了 reader 线程，那种时候 `fork()` 也是不想引入的风险；另一半是
  Chromium 自己在 `zygote_host_impl_linux.cc` 已经 FATAL 得足够早、足够响。
* **老路没量**：没把 `chrome-sandbox` 装成 setuid 再跑一次（要 root 改 CEF 分发里的文件权限）。
  所以"命名空间不可用时那个 SUID helper 确实能顶上"这句在本篇里只是 Chromium 文档的说法，
  不是我们量到的。
* **renderer 崩了不报**这条已经做掉了：helper 实现了 `OnRenderProcessTerminated`，控制通道上新增
  `renderer-exited` 事件，宿主侧是 `ChromiumSession::renderer-exited`
  （`docs/guides/control-channel.md`）。实测 `kill -9` 掉最大的那个 `Seccomp=2` 子进程后，host 只多出
  `status=1 error=9` 这一行，其余一切照旧——正是本篇原本担心的「只表现为页面静止」。第 7 节把这条
  在同一次长跑里量了两次，并且量到了它给不出更多东西的原因：没有崩溃处理器（第 8 节）。
  反过来，**其它**沙箱子进程（utility、GPU、网络）死了仍然没有任何事件，CEF 那边没有对应的回调，
  这条留在 `docs/guides/control-channel.md` 的「还没做的」里。
* **崩溃归因没做**：要给"为什么"就得往分发里加一个 handler 可执行文件、一个 reporter id 和一个上报端，
  那是产品决定，不是这里能顺手补的实现（第 8 节）。
* 沙箱开着时的**逐帧渲染正确性**没比对过（截图、dmabuf 提交都发生在不沙箱的浏览器进程里，
  逻辑上无关，所以只比了第 5 节那些可观察数字）。
