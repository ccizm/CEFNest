# 那条 `*** stack smashing detected ***`：谁检查、检查了谁、为什么谁也没写坏谁的栈

日期：2026-10-23
问题：第四轮记下的两条"等决定"里剩下的那条（第一条 `impl-slot-audit` 在第五轮补掉了）。报告原文是
"helper 有一条先前就存在的偶发 `*** stack smashing detected ***`，本轮未定位"。要求：归因，能消就消掉。

结论先说：**不是栈被写坏。** 是 glibc 的哨兵检查在**我们自己的 `main()` 尾言**里跑了，而跑到那里的那个
进程是 zygote 的 fork-without-exec 子进程——它继承了 `main` 的帧（连同序言从 TLS 抄出来的那个 canary），
但退出时 `%fs:0x28` 里已经是另一个值。修法是让子进程不要走尾言：`_exit(exit_code)`。

## 1. 现场为什么一直归不了因

一跑长这样（`docs/testing/real-sites.md` §3 那张表记的历史数量）：页面照常加载、HTTP 200、js 往返全对，
stderr 里夹着两行

```
*** stack smashing detected ***: terminated
```

信息量只有这些：没有 pid、没有栈、没有帧地址。这里也没有崩溃处理器可以指望——`docs/internals/sandbox-policy.md` §8
说过，进程在 OS 层面死掉是宿主能看到的唯一事实。于是"某个 libcef 沙箱子进程自己 abort，代价为零"就成了
一个只能观察、不能证明的说法，2026-10-22 那轮就是这么记的。

要证明只有两条路：让死掉的进程自己报名，或者让它别死。

## 2. 让进程自己报名：一次绕弯，一次对的

**绕弯的那次**：`LD_PRELOAD` 一个假的 `__stack_chk_fail`（`/tmp/chkfail.c`），用 `dladdr` 报返回地址，
再按 `main` 的序言算出哨兵槽的位置把前后各 6 个 qword dump 出来。这条路能用，但脆：偏移是从反汇编抄来的
常数。当前这个二进制里 `main` 长这样（`objdump -d`）：

```
00000000000210e0 <main>:
   210ea:  sub    $0x4b8,%rsp
   210fa:  mov    %fs:0x28,%rax
   21103:  mov    %rax,0x4a8(%rsp)     <- 序言把 TLS 里的 canary 抄进自己的帧
   ...
   21c79:  sub    %fs:0x28,%rax        <- 尾言拿帧里那份和 TLS 现值比
   22383:  call   201c0 <__stack_chk_fail@plt>
```

加了那套探针之后编译出来的 `main` 不是这个尺寸：同一台机器上留下的两个二进制（`objdump -d`，
2026-10-23），带探针那份是 `sub $0x4a8,%rsp` + `mov %fs:0x28,%r15` + `mov %r15,0x498(%rsp)`，
现在这份是上面的 `0x4b8` + `0x4a8(%rsp)`。**改一次 `main` 就得重算一次偏移**；而且这些子进程
不可 dump，`/proc/self/exe` 读不出来。

**对的那次**：不用偏移。`main` 序言抄进帧里的值，就是 `%fs:0x28` 在 `main` 被进入那一刻的值——这是构造出来的
等式，不需要知道帧长什么样。所以在 `helper/src/main.cpp` 的 `main()` 里自己读两次就够了：

- 进 `main` 时读 `%fs:0x28` 存成全局（fork-without-exec 的孩子连这个全局一起继承）；
- `CefExecuteProcess()` 返回后再读一次，两个值打出来。

## 3. 量到的形状

CEF 154.0.34，payload 是 `https://cn.bing.com/search?q=CEF` + `wait 9000` + 一次 rAF 计数 + `js-expr document.title`，
三次跑（`/tmp/canary-on/run{1,2,3}.out`，沙箱 on，`helper: sandbox=on`）。每一次都是同一个形状，各两个子进程：

```
helper: canary entry pid=1 ppid=0 comm=cefnest-helper entry=0xa202d50cb632a900 now=0xa202d50cb632a900 unchanged
helper: canary subprocess-return pid=1 ppid=0 comm= entry=0xa202d50cb632a900 now=0x9c934ba86441ab00 CHANGED
*** stack smashing detected ***: terminated
helper: canary subprocess-return pid=1 ppid=0 comm= entry=0xa202d50cb632a900 now=0xa7f9ebea9cce1000 CHANGED
*** stack smashing detected ***: terminated
```

读得出四件事：

1. **进入时没换。** 三次跑里每一条 `entry` 报告都是 `unchanged`——包括 `comm=exe`、有真实 pid/ppid
   的那些被 `exec` 起来的进程（探针只在 `main` 入口和 `CefExecuteProcess()` 返回处读，所以"进入时没换"
   是量到的，"全程没换"没有）。分叉只发生在没 exec 过的那条路上。
2. **它就是那个 abort 的进程。** `CHANGED` 行与 abort 行在文件里严格相邻：三次跑各 2 对，
   共 6 对 6（`run1` 的 44/45、46/47 行就是两组）。
3. **`pid=1 ppid=0`**：自己在 PID namespace 里，即 zygote 的 fork-without-exec 孩子；`comm` 空是非 dumpable。
   一次跑里两个这样的孩子，`now` 各不相同——新值是每次现取的，不是某个固定常数在跟我们对撞。
4. **同一次跑的 `entry` 行里，这个 `pid=1` 的孩子报的是 `cefnest-helper`。** 也就是说这个帧、这段
   `main()` 是我们自己的代码，不是 libcef 里的某个帧。

## 4. 剩下两种解释，各拿一个模型判掉

`glibc` 那行消息**分不清**"帧里的槽被改了"和"TLS 的值变了"——这两种状态在它看来是同一件事。
`/tmp/canarymodel.c` 三个模式分别构造：

| 模式 | 构造 | 实测 |
| --- | --- | --- |
| `fork` | 普通 `fork()`；再 `FORK_CHILD_RETURN=1` 让孩子**真的从 `main` 返回** | 父子 `%fs:0x28` 完全相同（`unchanged`），孩子从 `main` 返回也是 `signal=0 exit=0`——**fork 语义本身不动哨兵**，那个新值只能是这个孩子自己的 TLS 建立路径给的 |
| `poison` | 真越过局部缓冲区一个字，改掉**自己帧里**的槽 | `*** stack smashing detected ***: terminated`——真·越界的消息和现场那行一模一样，所以光凭消息不能定罪 |
| （主路径） | 帧里留合法值，TLS 换成别的（`MODEL_RETURN=1` 走 `return`，否则 `_exit`） | `return` → 同一行消息 + rc 134；`_exit` → rc 0，什么都没有 |

第三行就是修法的有效性证明，而且是确定性的：不需要 bing 那个触发器也能重跑。

### 4.1 那条路上确实有一次"给孩子重设栈"的动作（线索，未验证到 canary）

一次正常跑的 `ps`（2026-10-23，`./test/ci-all.sh` 中间一轮）：父进程只有
`cefnest-helper --ozone-platform=wayland`，而**每一个**孩子——`--type=zygote`、
`--type=gpu-process`、两个 `--type=utility`、`--type=renderer`——的命令行里都多了
`--change-stack-guard-on-fork=enable`，`libcef.so` 里这个开关名出现 1 次。也就是说 Chromium
自己认为 fork-without-exec 的孩子需要在孩子这一侧重新建立栈边界。这与我们量到的"孩子的 TLS
里多了一个新值"是同一条路径上的事，但**没有证据说就是这个开关写的 canary**：要把这条坐实，
得在触发器复现的那次跑里对照开关的两个取值，本轮没做到（§7）。

## 5. 修法：子进程不走尾言

```cpp
const int exit_code = CefExecuteProcess(main_args, app, nullptr);
if (exit_code >= 0) {
  // …（注释就是 §3 那四件事的压缩版）
  _exit(exit_code);
}
```

依据是 CEF 自己的文档，`third_party/cef/dist/include/cef_app.h:56-58`：对已识别的子进程，
`CefExecuteProcess` **"will block until the process should exit and then return the process exit code"**。
返回的那一刻，这个进程该做的做完了，帧上没有任何东西需要展开；`cef_app.h:73-75` 那句
"the application should exit immediately"（针对 `CefInitialize` 失败）是同一条纪律的另一半。

代价说清楚：`_exit` 不跑静态析构、不冲 stdio 缓冲。这条分支上 helper 自己在 `CefExecuteProcess` 之前
一行 stdout 都没写过（环境横幅在它后面），CEF 的日志走自己的 sink；而改之前那个进程是**被 abort 掉的**，
连原来的 `return exit_code` 也没走通，所以对任何真实行为来说这里是净增，不是净减。

## 6. 门禁从"报告"改成"红"

`test/ci-session.sh` 里这段以前是"报出来但不算失败"（`else ok "…a libcef sandbox child…"`），
因为不知道它是不是我们的问题。现在是：

```sh
aborts=$(grep -c 'stack smashing' "$RUN/demo.out" "$RUN/demo.log" | awk -F: '{s+=$2} END{print s+0}')
if [ "$aborts" = 0 ]; then ok "no subprocess aborted during the run"
else bad "$aborts stack-canary abort line(s): the zygote child left through main's epilogue"; fi
```

敢收紧的理由就是 §3：这条消息在这棵树里的唯一来源已经定到位并消掉了，再看见一次就是它回来了。
`docs/testing/real-sites.md` §3 里"每个站点两行"是**修前**二进制的历史测量，已经标注成历史。

## 7. 没量到的（别当成已知）

- **是整个 TLS block 被换，还是 `+0x28` 这个字被原地改写。** 探针后来加了 `%fs` 基座对比
  （`ARCH_GET_FS`，`same-block` / `NEW-BLOCK`），但加完之后的 bing A/B（4 轮 + 6 轮，control 与
  fixed 交替，共 20 次跑）**一个 `subprocess-return` 事件都没有**，所以这一维**没有答案**。
  对修法没影响：两种形状走尾言都会撞。
- **沙箱关掉以后还换不换。** 那次对照实验的输出没留下来（`/tmp/chkfail-run` 被后续 A/B 覆盖了），
  而触发器后来不再复现，所以现在**不能**声称"与沙箱无关"。§4 的 `fork` 模型只排除了"fork 语义自己会换"。
- **触发器为什么停了。** 同一台机器、同一份 payload：10:40 的三次跑每次两个子进程，11:06 起那 20 次
  一次没有（其间重编过一次探针二进制，加了 `%fs` 两个字段）。未知。这也是为什么 §5 的验证改用模型程序
  而不是在线 A/B——control 那半边连 abort 行都没出现过，说明它不是"修好了所以不报"，而是根本没走到。
- **zygote 具体在哪一行换的。** 没读 Chromium 源码，只量到现象。
- **别的 CEF 版本 / 别的机器。** 只有本机、CEF 154.0.34 minimal 这一组数据。

## 8. 两条工具约束（下次别再踩）

- **renderer 回调里不能插探针。** 在 `OnContextCreated` 里做 `fprintf` + 读 `/proc`，渲染进程立刻
  `demo: renderer exited status=2 error=11 11`，而且探针一行都没出来。要看子进程状态只能挂在
  browser 侧或自己的 `main()`。
- **不要依赖帧偏移。** 见 §2：偏移跟着编译走，非 dumpable 进程又读不到自身 exe。"和自己进入时的值比"
  这个写法既短又难错。
