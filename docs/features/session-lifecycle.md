# 退出路径：关窗口要停 helper，::closed 只发一次、且带上原因

日期：2026-10-20，§4–§6 补于 2026-10-21，§7 补于 2026-10-23
问题：任务 #27、#84、#85、#86。demo 里 `chromium_session_stop` 只有 header bar 上那颗「停止」按钮
能调到，窗口关掉它就当没发生；`demo.session` 又是个从来不 unref 的 static。于是每
跑一次，`$XDG_RUNTIME_DIR` 里多一个 `cefnest-helper-<uuid>.sock`——名字每次都是新
的 uuid，所以谁也不会回来收旧的。脚本化验证一晚上攒了 20 个。

## 1. 三条退出路径

`src/main.c` 现在在三个地方收口，都汇到同一个 `stop_everything()`：

| 路径 | 触发 | 谁接手 |
| --- | --- | --- |
| 点窗口关闭 / `gtk_window_close` | `close-request` | `window_close_request` |
| 脚本 `quit`、`CHROMIUM_DUMP` 定时器 | `GApplication::shutdown` | `app_shutdown` |
| Ctrl+C、`kill`（默认处置是直接死） | `g_unix_signal_add` | `signal_quit` → 变成一次 quit |

顺序上有讲究：`close-request` 里必须先 `stop_everything()` 再清指针。停会话会发
`::closed`，而 `::closed` 的处理函数要动 toolbar 的敏感度——`return FALSE` 之后
GtkApplicationWindow 的默认 handler 就把窗口连同 view、compositor 一起销毁了，那时
再发信号就是踩已经没了的 widget。所以：

```c
static gboolean
window_close_request (GtkWindow *window, gpointer data)
{
  stop_everything ();
  forget_widgets ();
  return FALSE;
}
```

`forget_widgets()` 存在的理由：window、view、toolbar 都是**借来的**指针（app 持
window，box 持 view），关掉就没了。留着旧值，下一次 `notify::surface` 就会拿它去
`g_object_new` 式的断言。`update_title()`、`session_changed()`、`session_closed()`
现在都先判空。

signals 那一路是实测加上去的：`g_unix_signal_add (SIGINT/SIGTERM, …)` 之前，
`kill` 掉的进程连 socket 都不会解绑——GApplication 并不会像文档让人以为的那样替
你处理信号。

## 2. ::closed 只发一次

`mark_closed()` 原来的门槛是 `!running && !child`。问题是 `child` 要等
`child_waited` 回调里才清，于是「主动停」这条路必然走两遍：`stop` 里发一次，helper
真被 reap 掉之后 `child_waited` 再发一次。

加了 `closed_emitted`，语义是「这一轮 start 已经报告过结束了」：

* `init` 置 TRUE——没开始过，也就没有结束可报；
* `start` 置 FALSE；
* `dispose` 置 TRUE——已经在拆了，迟到的 `::closed` 只会打在空的 toolbar 上。

A/B 实测（fake helper 自己在 0.7 s 后发 `{"event":"closed"}` 然后退出，一条路能数出
三次结束）：

```
去掉 closed_emitted：grep -c 'helper stopped' → 3
加上：             grep -c 'helper stopped' → 1
```

三条都是真的：`helper closed the browser`（事件）、`control channel closed`
（读循环 EOF）、`process exited`（child_waited）。宿主只该被告知一次。

## 3. 验证

用不着真 CEF，`test/fake-helper.py` 会说控制协议：

```sh
GDK_BACKEND=wayland \
CHROMIUM_HELPER=$PWD/test/fake-helper.py \
CHROMIUM_SESSION_SCRIPT=$PWD/test/close.session \
builddir/src/cefnest-demo
```

`close.session` 就是 load → wait 800 → **close**，脚本语法里新加的这一步走的是和
手点关闭完全相同的路（`gtk_window_close`）。四条退出路径逐个跑：

| 场景 | 结果 |
| --- | --- |
| `close` + 会话 | 退出码 0，`helper stopped` 1 次，遗留 socket 0 个 |
| `kill -TERM` | 同上（修复前：0 次，遗留 +1） |
| `kill -INT`（Ctrl+C） | 同上 |
| helper 自己退出 | `helper stopped` 1 次（修复前 3 次），遗留 0 个 |
| 纯客户端（shm-client）关窗 | 无游离进程 |

`test/seat-selftest` 六档回归照旧全过，24 步跑完，fd 计数不变。

## 4. helper 在 ready 之前就死了：脚本化运行要立刻失败

`XDG_CACHE_HOME` 给两个实例共用时，第二个拿不到那份 profile：helper 自己先说一句
「正在现有的浏览器会话中打开。」，然后是

```
helper: CefInitialize failed
session: helper exited with status 1
session: helper stopped (process exited)
```

先否证一条旧记录。早前记的是"共享 cache 目录会让**第一个**实例的 helper 跟着
signal 11 死掉"——在当前 build 上复现不了：B 失败的那段时间里 A 一直在出帧
（`js` 读回 `requestAnimationFrame` 计数，见本节末），退出也干净。那次崩溃是
b93be96 帧时钟回归的连带，不是 profile 共享的后果。库这一侧的语义本来就是对的：
`::closed` 发了一次，socket 也收了。

真正的缺陷在宿主。B 的转录里一条 `demo: script` 都没有：demo 把脚本读了进来，一个
步骤都没派出去，然后把窗口一直开着——最后是 harness 杀掉 compositor 才结束的。对人
来说那是"窗口在那儿但什么也没发生"，对无人值守的一跑来说那是把一次两秒的失败放大成
`PAYLOAD_TIMEOUT`（默认 900 秒）。

原因在两条时间线的接缝上：`start_session_script()` 只在**没有** session 的时候自己上
表（`if (!demo.session) g_timeout_add (800, script_wait_done, NULL)`），有 session 时
第一步等的是 `::ready`；helper 没起来就永远没有 ready，而 `session_closed()` 当时只
把 toolbar 的敏感度和标题收拾了一下。脚本停在 index 0，没有第二个人会往前走它。

修法是把"这条运行还有步骤没派出去"当成失败来报告：

```c
static void
session_closed (ChromiumSession *session, gpointer data)
{ /* 敏感度、标题 …… */
  script_note_helper_gone ();
}
```

`script_helper_gone()` 挂在 idle 上而不是当场做完，这一条是 `close` 逼出来的：它发的
`::closed` 就在**同一次 main loop 迭代**里到（`gtk_window_close` → `close-request` →
`stop_everything` → 停会话），而那一步的 `script_finish()` 要到循环末尾才执行——当场
判断就会把一次按脚本结束的正常运行读成失败。挂到 idle 上等它被看时，脚本已经清空。
`dump` 是 `dump_view()` 里直接 quit，idle 排不进去；`quit` 本来就先把脚本收干净再退。
一次已经在退出的运行（信号、shutdown）也不会再有一个活着的 main loop 去跑那个 idle，
这个答案同样对：已经在结束的运行不是被 helper 抛弃的运行。实测 `close` 与 `dump` 末行
的脚本修复前后都是 0，两条都没被误伤。

判据用 `index < steps->len` 而不是 `steps != NULL`：末行是 `wait`/`dump` 时索引已经
走到末尾，那种运行剩下的是一个定时器，它照常会以 0 收尾，不该算成失败。

`main()` 交出的也不再是写死的 0，而是 `demo.exit_status`——任何内部判定要传到
harness，先得有人把它带出去。

实测（nested 会话里；helper 换成带 `FAKE_HELPER_DIE_BEFORE_READY=1` 的
`test/fake-helper.py`——连上 socket、一个字不发就 `exit(1)`，正是 `CefInitialize`
失败那个形状；脚本 `load` → `js` → `quit`）：

| 脚本末行 | helper | 修复前 | 修复后 |
| --- | --- | --- | --- |
| `quit` | 假 helper，死在 ready 前 | `timeout 20` 那一跑 rc=124：窗口开着，一步没走 | 整跑约 2 s，状态 1 |
| `quit` | 真 Chromium，profile 被另一实例占着 | 挂到 5 分钟，靠 harness 杀 compositor 才结束 | 状态 1，且报告与 `helper exited` 打在**同一毫秒** |
| `close` | 假 helper（活着） | 0 | 0 |
| `dump` | 假 helper（活着） | 0 | 0 |
| `quit` | 真 Chromium（独占 profile，proof 那一跑） | 0 | 0 |

转录里多出来的那一行：

```
demo: the helper went away with 3 of 3 script step(s) still to run, so the run ends here
```

它现在有门禁：proof 模式的第二跑就是这道题，钉的是退出码 1 和这行转录，见
`docs/testing/headless-ci.md` §4。

共用 profile 那一跑里 A 侧的读数——这是"第一个实例会被带死"那条旧记录被否证的地方：
helper ready 之后 `js` 三次读回页面里的 `requestAnimationFrame` 计数，**1 → 242 →
722**；B 在第二个读数之后才 `CefInitialize failed`，而 A 在其后那 8 秒里照常
**60 帧/秒**（480 帧 / 8.0 s），最后自己退出、状态 0，转录里没有 signal 11。

另外两条 `::closed` 的来路这一跑没测出东西：`FAKE_HELPER_QUIT_AFTER=1000` 那两个脚本
实测都在 helper 自己关之前就跑到了头——转录里只有 `helper stopped (stopped by the host)`，
报告那行没出现，`rc=0`。它们与 `process exited` 进的是同一个入口，行为应当相同。
当时 `::closed` 还不带原因，"宿主自己停的"、"helper 自己关的"、"进程没了"这三种报告
出来是同一句话；下一节把它给出去了，并且把 `QUIT_AFTER` 用一条留了 `wait` 的脚本真正
测了出来。

## 5. ::closed 带上原因（任务 #85）

§4 那行只能说"helper 走了"，因为 `mark_closed()` 的 `why` 只进了日志——而 `g_message`
默认不显示，宿主没设 `G_MESSAGES_DEBUG` 就是一个字也看不见。现在它是信号参数：

```c
g_signal_emit (self, signals[SIG_CLOSED], 0, why);
```

四个值就是调用方原本写下的那几句话，原样带出去：`stopped by the host`、
`helper closed the browser`、`control channel closed`、`process exited`。用字符串而不是
枚举，跟这套 API 里其他"是哪一种"一致（`::download-end` 的 `state`、
`::permission-dismissed` 的 `result` 同样是字符串），将来多一种来路也不必改签名。
demo 拿它把 §4 那行补全：

```
demo: the helper went away (process exited) with 3 of 3 script step(s) still to run, so the run ends here
```

信号参数只保证在发射期间活着，而那个判断要等一个 idle，所以走的是 `g_strdup` 的副本。

四种来路都实测过（nested 会话里，脚本 `load` → `js` → `quit`，helper 用
`test/fake-helper.py`，只有第二跑换成 `/bin/false`）：

| 形状 | 报告的原因 | 还剩几步 | rc |
| --- | --- | --- | --- |
| 连上就退（`FAKE_HELPER_DIE_BEFORE_READY=1`） | `control channel closed` **或** `process exited` | 3/3 | 1 |
| `/bin/false`：从头到尾没连上 | `process exited` | 3/3 | 1 |
| 活着时自己发 `closed`（`FAKE_HELPER_QUIT_AFTER=1000`，脚本里留一条 `wait 4000`） | `helper closed the browser` | 2/5 | 1 |
| 脚本以 `close` 或 `dump` 收尾 | `stopped by the host` | 没有 | 0 |

第一行那个"或"是量出来的：同一个形状，一次报 `control channel closed`、另一次报
`process exited`；而 `SIGKILL`、`SIGTERM` 那两跑的转录都是 `helper stopped (control
channel closed)` 后面**紧跟**`helper killed by signal 9`/`15`——两条回调落在同一毫秒，
谁先到看运气。socket 挂断和子进程被收走是同一件事的两面，`mark_closed()` 只发第一次，
所以宿主该问的是"是不是我自己停的"，而不是"它具体怎么死的"；真要死因，`child_waited`
那两行日志才是地方。门禁因此钉的是"死亡报告不说谎"（前两个二选一，且 `rc=1`），
不是钉死其中一个。

proof 模式现在用一条 `broken_run` 函数把这几形状各跑一遍，钉退出码 1 与转录里的原因；
它还有第四跑，就是下一节。

## 6. 没有会话的脚本化运行也以 1 结束（任务 #86）

§5 那张表里"会话从没活过"这一格是空的，不是漏了：`CHROMIUM_HELPER=/no/such/helper`
时 `chromium_session_start` 直接失败（`demo: no helper binary at …`），会话被清空，
于是**没有 `::closed` 可发**——它从没开始过。当时量到的是更难看的一种：脚本里每条
页面命令只回一句 `demo: 'load …' needs a Chromium helper session` 就 `continue`，
索引照往前走，`quit` 一到就以 **0** 结束（实测 `rc=0`、两条警告、零条 `script ok`）。

比 §4 那个挂住更糟：**它看着像过了**。挂住的至少会红。

修法是把那句警告从"跳过这一步"改成"这一跑不可能做到它被要求做的事"：

```c
      if (!demo.session && (… || !strncmp (line, "perm-", 5)))
        {
          g_printerr ("demo: '%s' needs a Chromium helper session, so the run ends here\n", line);
          script_finish ();
          demo.exit_status = 1;
          g_application_quit (G_APPLICATION (demo.app));
          return;
        }
```

三个细节都是读代码时盯出来的：

* **先打印再 `script_finish()`**。`line` 指的是 `steps` 那块 `GPtrArray` 里的字符串，
  而 `script_finish()` 就是来 `g_ptr_array_unref` 它的——顺序反过来打印的是已经自由了的内存。
* **这里不能挂 idle**。§4 那处非挂不可，因为 `close` 那一步自己就是发出 `::closed` 的人；
  这一处是脚本引擎在派发步骤的当下，没有谁在 unwind，直接退出就行。
* **`perm-` 原来不在这张名单里**。按代码读出来的路径，它过去会一路走到
  `chromium_session_permission_respond` 的 `g_return_if_fail (CHROMIUM_IS_SESSION (self))`，
  也就是一条 CRITICAL 之后直接返回、脚本照旧走到 0 收尾——这一句是读出来的，没有单独复现
  （改完之后那条旧路径就不存在了）。补进名单后实测同一条脚本 `perm-deny 1` → `rc=1`。

客户端那一侧没被波及：`hover`/`click`/`drag`/`wheel`/`shot`/`dump`/`surfaces`/`drop-in*`/
`close` 仍然不需要会话，`pixel-gate.sh` 与 `dnd-gate.sh` 两条纯客户端门禁在同一块
headless 显示器上重跑，全过、断言数不变。

`proof` 的第四跑钉的就是这条：helper 换成一个不存在的路径，脚本 `load` → `js` → `quit`，
断言 `rc=1` 与那句 `needs a Chromium helper session, so the run ends here`。

## 7. helper 那一侧也收信号：装在 `CefInitialize()` 之后

问题：2026-10-23 那份复核指出 §1 那张表只讲了**宿主**怎么处理信号，而 helper 自己
`grep` 不到 `signal`/`sigaction`/`SIGTERM`。上一轮报告（S5）里"信号处理"那一半其实从未落地。
`systemctl stop`、桌面会话注销、以及任何人手一次 `kill`，用的都是 SIGTERM/SIGHUP/SIGINT，
默认处置 = 直接死：不跑 `Control::Shutdown()`、不跑 `CefShutdown()`，profile 里留下一个
`SingletonLock`，下一个用同一 profile 的 helper 会当成"已有一个浏览器会话在跑"。

补完之后形状是这样（`helper/src/main.cpp`）：

| 部分 | 做什么 | 为什么必须是它 |
| --- | --- | --- |
| `OnShutdownSignal()` | 只往管道里写**一个字节**（信号号），保存并恢复 `errno` | `CefPostTask()` 要拿锁，信号上下文里不能调；`write()` 单字节是 async-signal-safe |
| `SignalWatcher::Start()` | `pipe2(O_CLOEXEC)` + `sigaction{SA_RESTART}` 装 SIGINT/SIGTERM/SIGHUP + 起读线程 | 自管道是"从信号里回到正常线程"的标准跳板 |
| 读线程 → `SignalTask` | 读到字节才 `CefPostTask(TID_UI, …)`，一次信号只announce一次 | 真正的工作必须在 UI 线程上做 |
| `SignalTask::Execute()` | `CloseEveryBrowser()`，不是 `QuitOnce()` | 关掉最后一个 slot 时 `OnBeforeClose` 自己会 quit；在这里 quit 会让浏览器挂在一个没人应答的通道上 |
| `main()` 尾巴 | `CefRunMessageLoop()` 返回后 `signals.Stop()` → `control.Shutdown()` → `CefShutdown()` | 两个线程（watcher、reader）都必须在 `CefShutdown()` **之前**收干净，否则它们能把任务投给一个正在消失的 CEF |

两处只有量一次才知道的事：

* **`CefInitialize()` 会把这三个信号改回 `SIG_DFL`。** 第一版把 `signals.Start()` 放在
  `CefInitialize()` 之前，门禁跑出来是退出码 143（TERM）/130（INT），转录里没有
  `caught signal`——不是 handler 没干活，是 handler 已经不在了。strace 看得很清楚：
  我们装好之后约 2 ms，`rt_sigaction(SIGTERM, {SIG_DFL}, …)` 三连发来。所以现在是
  **`CefInitialize()` 返回之后**才 `Start()`，`/proc/PID/status` 里 `SigCgt` 读出
  `0000000100014003`（bit 1/2/15/17/27…，SIGHUP、SIGINT、SIGTERM 都在内）。
* **`pipe2(fds, O_CLOEXEC \| O_NONBLOCK)` 是错的**：NONBLOCK 加在两个 fd 上，watcher 第一次
  `read()` 拿到空管道就返回 `EAGAIN`，当致命错误退出线程，信号字节从此写进一个没人读的管道。
  只有**写端**该非阻塞（管道满时绝不能阻塞信号处理器），读端必须老老实实在 `read()` 里等。

量的过程里翻出两个原有缺陷，都不是信号路径自己的问题，但不修它们这条路径走不完：

1. **第一个 tab 永远关不掉。** `HelperWindowDelegate::OnWindowCreated()` 把 view
   `AddChildView()` 进窗口之后，还继续把 `browser_view_` 攥在手里——于是同一个 view 有两个
   所有者，Alloy 对 `CloseBrowser(true)` 的读法就变成「把这个**窗口**关掉」而不是「把这个
   **浏览器**关掉」。实测：slot 1 的 `CloseBrowser(true)` 返回正常但 `OnBeforeClose` 永不到来
   （没有 `::browser-closed`），而后建的 tab 个个能关。信号处理加好之后仍然停在
   `helper: browser 1 closing` 不动，就是被它挡住——`CloseEveryBrowser()` 关不掉最后一个
   slot，`QuitOnce()` 就永远不会发生。修法是在 `AddChildView()` 之后把 delegate 里那份引用放下。
2. **helper 自报"我要走了"之后，宿主仍然 SIGKILL 它。** `{"event":"closed"}` 是 helper 退出路径
   的第一句（`Control::Shutdown()` 紧随其后），宿主收到它就把这一跑判为结束；而 demo 收尾时
   `chromium_session_dispose()` 无条件 `g_subprocess_force_exit()`，正好砍在 `CefShutdown()`
   走到一半。profile lock 那时已经放了，所以症状只是 `main()` 的尾巴再也不跑。
   `src/chromium-session.c` 现在记一个 `helper_is_exiting`：helper 自己报的 `closed`、或控制通道
   被对端关掉（EOF），都算它已经宣布在退出，`stop()`/`dispose()` 就不动手——留下的
   `child_waited` 持有引用，进程该收还是收。显式的宿主停止（通道还开着）照旧 force-exit。

门禁：`test/helper-signal-gate.sh`，两个信号各跑一遍完整场景（TERM、HUP），
每边 9 条断言 = 18 条。它在 nested session 里跑（`./test/ci-session.sh
./test/helper-signal-gate.sh`），已进 `test/ci-all.sh` 的 `signal` 一行。它用的是
**hosted** 模式：standalone 在那套 headless 下 `CefInitialize()` 不返回
（`docs/internals/sandbox-policy.md` §9，当前与上一版二进制都试过），所以 helper 必须按真宿主
的方式拉起来。六条值得单独说的判据：

* 动手之前先读 `/proc/PID/status` 的 `SigCgt`，确认这个信号**确实被捕获着**。少了这一步，
  "handler 没装上"和"转录里少一行日志"在断言里长一个样。
* 断言的是"跑到 `main()` 的尾巴"（`helper: exited` 在 `CefShutdown()` 之后）和
  "profile 的 `SingletonLock` 没了"——后者是这条 bug 原本的用户可见症状。
* 宿主必须活到子进程被 reap，并报出 `session: helper exited with status 0`。这一条不是
  重复：`helper: exited` 只是一行 stderr，宿主自己的 wait 状态才能区分干净退出和
  打印完尾巴又被 GDK 结束的进程。
* demo 自己印出 `helper process reaped`，即真的收到了 `::helper-exited`。只看 helper
  的转录分不清"宿主等到了这个信号"和"宿主一直没退、最后被门禁杀掉"。
* demo 随后**自己**退出。等信号如果退化成永远不等，这道门在 5 秒后红，而不是把一次
  挂起留给整轮 CI 的超时。
* 转录里不许出现 `session: helper killed by signal`：那是宿主自己观察到进程被信号打死。

**2026-10-23 补的一条门禁竞态，以及把它修在 API 上**：原来的 session 脚本是
`load` → `wait 12000` → `quit`。helper 在 `wait` 还没走完时退出，
`script_helper_gone` 看见还有 pending step，就按"这一跑失败"直接
`g_application_quit()`；demo 一退，它持有的嵌套 compositor 消失，helper 的 GDK
打出 `Lost connection to Wayland compositor.` 并把进程结束在 `CefShutdown()` 尾部
之前。全量加单独重跑共 11 次，2 次红（约 1/5），报错却是
`'helper: exited' missing — CefShutdown did not run`——把宿主拆 compositor 的时序
误报成了 CefShutdown 没跑。

先试过让脚本在 `load` 后就结束（脚本下标到末尾，`::closed` 不触发那条失败路径），
但脚本引擎在末尾会自己 `quit`，信号还没落到 helper 上宿主就走了，门禁反而稳定地
报"页面没装上"。真正的问题是 API 少了一个时刻：`::closed` 只说明**页面**没了，
helper 进程还在 `CefShutdown()` 里用它拿到的 Wayland 连接。于是新增
`::helper-exited`（无参数）：`child_waited()` 收走当前子进程、记录退出状态、发完
`::closed` 之后才发一次。demo 把"脚本被截断"的报告留在 `::closed`，退出推迟到
`::helper-exited`；`wait`/`quit` 因此可以留在脚本里，门禁真正走到这条路径。转录里
的顺序是 `helper stopped (helper closed the browser)` → `the helper went away …` →
`helper: exited` → `helper exited with status 0` → `helper process reaped`。改完连跑
10 次全绿；此前同一形状 11 次里红 2 次。

**边界**：`::closed` 仍然早于进程退出，这是协议原来的语义，没有改。宿主如果自己拥有
显示，就该在 `::helper-exited` 里拆；仍在 `::closed` 里同步销毁 compositor 的宿主会
重演这条竞态。示例宿主在 `::closed` 里保留窗口，demo 现在等 `::helper-exited`，两边
都不踩。

没修的一条：`kill -9` 依旧什么都来不及做（§8）。

## 8. 还没做的

* **compositor 自己的 `wayland-N` + `wayland-N.lock` 仍然留在** `$XDG_RUNTIME_DIR`。
  这个不补：只有 `nested_compositor_dispose` 跑到才会 `wl_display_destroy` → unlink，
  而 GTK 在退进程时并不 finalize widget 树（实测 `close` 路径 view 和 compositor 的
  dispose 都不进）。名字是有界的、会被复用——下一个进程发现 lock 能 flock 成功就
  直接把它抢过来用，所以不会像 uuid 名字那样无限涨。
* `chromium_session_stop` 结尾把 `stopping` 又放回 FALSE，理论上让迟到的
  `child_waited` 有机会把「Operation cancelled」当真错误打出来；目前所有退出路径
  都没复现，先不动。
* 崩在 `kill -9` 下仍会留一个 helper socket——没机会执行任何清理。
* `demo.exit_status` 管的是**这一跑做完没有**，不是**画对没有**。一条 `load` → `quit`
  的脚本，页面是白是红都是 0；看像素永远是 `test/pixel-gate.sh` 那条路的事
  （`docs/internals/frame-clock-pacing.md` §2 那条"CSS 不需要一帧"的教训）。别把 rc=0 读成"页面
  看着对"。
* `::closed` 带的是话术而不是枚举，宿主只能整串比。真出现要按位判断的宿主，再包一层
  `chromium_session_get_close_reason()` 或者换 enum，现在不猜需求。
