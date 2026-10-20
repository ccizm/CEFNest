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
命名空间取自 `/proc/PID/ns/`；去掉 `chromium-demo` 自己，helper 一家 9 个进程）：

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

**这些叶子是什么类型，给不出标签。** Chromium 给子进程重写了 argv，`/proc/PID/cmdline`
只剩 1 项，`--type=` 读不出来；`/proc/PID/comm` 全都是 `chromium-helper`。上面这张表只说
"谁是谁的子进程、进没进 seccomp、在哪个命名空间"，这三件事是量出来的。

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

先跑独立模式的 helper（`chromium-helper https://example.com/`，它拿到的是宿主的 `wayland-0`，
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
`No usable sandbox!` 上，没试过——那句 FATAL 里专门提到了 Ubuntu 23.10+，看语气是同一条出口，
但这一条留给需要它的人去量。没去动宿主自己的 sysctl。

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

* **不需要 setuid 安装步骤。** 目标宿主只要有非特权用户命名空间，什么都不用装。
* 宿主禁了 userns（Ubuntu 23.10+ 的 AppArmor 限制是最常见的一种）时，那句 FATAL 链接的两篇
  Chromium 文档各自指一条路，加上它自己给的兜底，一共三条可选：
  给这个应用单独加 AppArmor profile（推荐，粒度是一个应用）；把 `chrome-sandbox` 装成
  `chmod 4755`（要 root，是老路，而且给了整机一个 setuid 靶子）；或
  `CHROMIUM_HELPER_NO_SANDBOX=1`（明确放弃沙箱，页面代码从此以宿主用户身份跑）。
  **不要**为了这个组件去关整机的 `apparmor_restrict_unprivileged_userns`。
* 沙箱保护的是页面代码所在的 renderer，**不是** helper 整体。浏览器进程永远不沙箱，
  因为它握着 Wayland 连接和控制通道；把"helper 进程"当成一个可信边界是错的，
  正确的边界是"renderer 里的东西都不可信"。
* `CHROMIUM_HELPER_NO_SANDBOX=1` 在启动横幅里留了痕（`sandbox=OFF (…)`），转录里能看到。

## 7. 还没做的

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
  （`docs/control-channel.md`）。实测 `kill -9` 掉最大的那个 `Seccomp=2` 子进程后，host 只多出
  `status=1 error=9` 这一行，其余一切照旧——正是本篇原本担心的「只表现为页面静止」。
  反过来，**其它**沙箱子进程（utility、GPU、网络）死了仍然没有任何事件，CEF 那边没有对应的回调，
  这条留在 `docs/control-channel.md` 的「还没做的」里。
* 沙箱开着时的**逐帧渲染正确性**没比对过（截图、dmabuf 提交都发生在不沙箱的浏览器进程里，
  逻辑上无关，所以只比了第 5 节那些可观察数字）。
