# headless CI 会话：把门禁搬到没人看的显示器上

日期：2026-10-21
问题：P4 需要一个能无人值守跑门禁的地方。本仓库的门禁全都"开真窗口"——在真桌面上
跑等于把窗口糊在使用者脸上，所以它们一直是"文档化的手动门禁"。
结果：`test/ci-session.sh` 起一个 nested mutter（GNOME Shell 48.7 的 `--headless`
模式），把任意命令塞进那个 Wayland 会话里跑；`test/ci-all.sh` 在它上面把 proof、
`meson test` 和九条门禁串成一条命令的发布闸门。不装包、不用 sudo、跑完不留进程；
九条门禁全绿，唯一要改的是 `menu-gate.sh` 里那两条"弹层上了屏"的断言——那块显示器
没有屏可上，于是它们报成 skip（§5）。新加的 `test/pixel-gate.sh` 是第一条不看日志、
只看像素的门禁，它在两块显示器上量到同一串颜色；把它第一次在真桌面上读到的纯色当成
"显示器的性质"是这次记录里的一个错，真相是组件自己的一条回归，§5.1 与
`docs/internals/frame-clock-pacing.md` §9 记的全套过程。

## 1. 底座：这台机器上现成有什么

- `gnome-shell --help` 里有 `--nested`、`--headless`、`--wayland-display=<name>`、
  `--display-server`：`--headless` 就是"没有输出设备的 mutter"，正是 CI 需要的显示器。
- `dbus-run-session` 在。
- **没有** cage / weston / sway / labwc / openbox。Xephyr、Xvfb 有，但那是 X11，
  而这条路线的立身之本是原生 Wayland。

## 2. 隔离不是整洁，是安全

第一次试跑把 `gnome-shell --headless` 直接起在真实 HOME 上：它连上了宿主会话的
D-Bus，激活了 nautilus 与 Tracker3 文件索引器，还自动重排了应用网格（57 个图标被
缩放）。一次"无人值守"的跑动就这样动了正在用这台机器的人的桌面。

现在每次跑都给它一套自己的 HOME、`XDG_CONFIG/CACHE/DATA/STATE`、
`XDG_RUNTIME_DIR`（chmod 700）和它自己的 D-Bus bus（`dbus-run-session`）。宿主的
`WAYLAND_DISPLAY` 与 `DISPLAY` 用 `env -u` 显式抹掉，否则 shell 会去连宿主的显示。
里面那些服务要么安静地失败，要么索引一棵空目录树。

## 3. 生命周期

- `setsid --fork` 让 compositor 成为自己会话的组长。这台机器的 util-linux 2.41.5
  没有 `setsid --write-pid`，所以由一个临时 wrapper 脚本自己 `printf %s $$` 落
  pid——那个 pid 同时是 PGID 和 SID，也是整棵嵌套进程树唯一的把手。
- **显示器名字是宿主上的全局命名空间**。跑法不对的时候会留下一个孤儿嵌套会话：
  这次就撞上一个 51 分钟前被打断（trap 没机会跑）的会话，它还占着
  `cefnest-ci`，下一个 `gnome-shell --wayland-display=cefnest-ci` 直接
  "Nested session is already running. Exiting."。所以默认名字带 pid
  （`cefnest-ci-$$`），两次跑动互不抢占；要共用一块显示器再用
  `CI_WAYLAND_DISPLAY` 显式覆盖。
- 等 socket 出现（`$XDG_RUNTIME_DIR/$NAME`，最多 80 秒）；等不到就把 compositor 的
  日志 tail 出来再退出。
- 收尾：`kill -TERM -$GS` 打给整个进程组（bus、portal、gvfsd 一起走），轮询
  `pgrep -g` 确认死干净，然后 `chmod -R u+rwX` + `rm -rf`。GVFS 的挂载点会挡住
  `rm`，清不掉就打印一行 note 而不是把整跑判失败。
- `trap cleanup EXIT`：任何失败路径都不会留下一个后台 mutter。

## 4. 两种模式

```bash
./test/ci-session.sh                          # 自检：证明这块显示器能用
./test/ci-session.sh ./test/find-gate.sh      # 把任意门禁塞进去跑
./test/ci-session.sh meson test -C builddir   # 单元侧同理
```

**proof（默认）**：起 demo 打 `test/dnd-page.html`，断言三层——嵌套 compositor 拿到
了 `WAYLAND_DISPLAY=`、页面 `HTTP 200`、`js` 从 Blink 读回页面标题（脚本步骤的 id
按脚本行计数，所以是匹配而不是钉死）。然后统计 `stack smashing` 行数：一个沙箱化的
libcef 子进程偶尔会 abort（strace 定位到是 zygote 的 fork-without-exec 子进程在自己
namespace 里 `tgkill(1, 1, SIGABRT)`，而我们自己的缓冲区都有边界检查），它不影响
前面三条，于是**报告而不失败**——这里没有崩溃处理器可以修或关，见
`docs/internals/sandbox-policy.md` §8。

最后是"这一跑做不到它被要求做的事"那四跑（一条 `broken_run` 函数）：同一个脚本，helper
分别换成①连上 socket 就 `exit(1)`、什么都不发的 `test/fake-helper.py`（`CefInitialize`
失败就是这个形状）、②`/bin/false`（从头到尾没连上）、③活着时自己发 `{"event":"closed"}`
的 fake（脚本里留一条 `wait 4000`，让它死在半路）、④路径根本不存在（会话从没起来，
`::closed` 也就没得发）。每跑钉两条：退出码必须是 **1**，转录那一行必须点出原因——①②③
是 `::closed` 给的原因，④是脚本引擎自己那句 "needs a Chromium helper session"。①的原因
实测会在 `control channel closed` 与 `process exited` 之间跳（两条回调同一毫秒到，谁先进
去看运气），所以钉的是二选一；②③④各有唯一答案。无人值守的一跑只有两种结局——退出码与
超时——所以一个真失败如果只能以超时出现，它等于把自己伪装成"这一跑有点慢"：2 秒的事会被
读成 900 秒；而比超时更糟的是**退出码 0**——④在修之前就是这个形状。修它们的三条见
`docs/features/session-lifecycle.md` §4–§6。

**payload**：`"$@"` 原样在会话里跑。注入的是 `WAYLAND_DISPLAY`、
`GDK_BACKEND=wayland`、`CEFNEST_HEADLESS=1`、隔离的 HOME/XDG/runtime 目录，以及从
compositor 自己的 `/proc/<pid>/environ` 里取出的 bus 地址（这样门户走会话内那条 bus，
而不是宿主的），`CHROMIUM_HELPER` 指向真 helper，超时 `PAYLOAD_TIMEOUT`（默认 900s）。
payload 的退出码就是这一跑的退出码；失败时把每一跑的 stdout/stderr 都 tail 出来
（demo、payload，加上 `die`/`exited`/`quit`/`nostart` 那四跑）。

`CEFNEST_HEADLESS=1` 是给门禁的"这块显示器没有输出设备"的声明，见 §5：断言"某块
surface 真的上了屏"的门禁读它，把那几条报成 skip 并把数量写进总结行，而不是报一个
会让人去查产品的 FAIL。

一块显示器可以喂很多门禁——起一次 nested mutter 比每个门禁起一次省十几秒：

```bash
./test/ci-session.sh bash -c './test/find-gate.sh && ./test/menu-gate.sh'
```

### 全套：`test/ci-all.sh`

发布闸门是**一条命令**，不是一个循环脚本：proof、`meson test`、九条门禁按顺序跑，
每个 run 各起一个 nested mutter，跑完打印判定表，任何一条 FAIL 就整体非零退出。

```bash
./test/ci-all.sh                    # 全套
./test/ci-all.sh find menu          # 只点名跑两条
KEEP_LOGS=1 ./test/ci-all.sh        # 保留每个 run 的完整 transcript

真实站点巡检是 opt-in，不在默认七条里（外部站点会改版/限流）：

```bash
CI_ALL_REAL=1 ./test/ci-all.sh real
```

`real` 这一行由 `test/real-sites.sh` 自己为每站起 `ci-session.sh`；`ci-all` 不再套外层
session。产物和五站表见 `docs/testing/real-sites.md`。
```

每个 run 单独起会话，是拿 ~10s/条换"挂掉的那条只烧自己那一行"：塞进同一块显示器时，
一条卡死或把外层会话弄脏，后面所有门禁的判定就一起没了——一套只能回答"里面某个地方
挂了"的门禁不是门禁。表里的 `skip` 也单独占一列：门禁的 skip 只在**这块显示器证明
不了**时发生（§5），所以绿线后面必须能把跳过条数数出来。

## 5. 它证明什么，不证明什么

证明：真窗口能在一块真 Wayland 显示器上映射；真 Chromium 会话（`sandbox=on`）能起、
能加载、能回答 JS；`test/*.sh` 那套门禁不需要为 headless 加分支就能跑——除了断言
"某块 surface 上了屏"的那两条（下面「上屏」那条）。还证明**页面的像素真的到了组件里**：
`test/pixel-gate.sh` 在这块显示器上量到的是逐字节相符的颜色，而且**在真桌面上量到
的是同一串数字**（下一条）。

### 5.1 像素两侧一致——以及一条差点把回归读成显示器性质的 skip

`test/pixel-page.html` 四块纯色 → 缓冲里 18,52,86 / 200,50,60 / 43,138,62 /
232,232,232，合成后的 widget 和原封不动的 client texture 两份都一样；headless 与
真桌面各跑一次，逐字节相同，所以这条门禁两侧都是全量断言、一条 skip 都没有。

这里要记一段**当时读错了**。这份门禁提交那天，真桌面侧的 `dump` 恒为 `#101828`
一整块纯色（"清了却没画过"的缓冲），页面自己的 `requestAnimationFrame` 计数 4.4s
里停在 3～4 次，同一次运行 `js` 那步照旧回答 `visibilityState=visible`、
`backgroundColor=rgb(255, 0, 0)`、`innerWidth=1000`——但 CSS 不需要一帧，所以那条
回答不能当"画对了"的证据。当时把纯色读成"这块显示器没有屏可上，所以没人看的窗口
没有像素"，还为此给真桌面写了一条 skip。那个 skip 差一点把真凶盖住：
**`#101828` 是组件自己的回归**——`bcbebc8`（`docs/internals/frame-clock-pacing.md` §8 那轮
改动）把 `fire_pending_frame_callbacks()` 从 after-paint 里删掉了，于是时钟只要还
回答，客户端的帧请求就永远没人答，真桌面整页冻住。把那一行加回去，同一台机器同一个
页面：rAF 从 4/4.5s 变成 252/4.5s，`dump` 从 `#101828` 变成页面自己的绿色，
`test/pixel-gate.sh` 两侧 12 条断言全过。
顺带把 §8 那句"遮挡的窗口只停 100ms"也量实了：把 demo 最大化再用一个全屏窗口整个
盖住，被盖住的那一侧 rAF 仍是 61.5/秒，缓冲 1829x982 照样出图、读回是画过的红。
两条教训都写在 `docs/internals/frame-clock-pacing.md` §9。

### 5.2 不证明

- **帧节拍**。headless mutter 没有输出设备，宿主 GDK 不再产生显示周期，客户端的帧
  由 `docs/internals/frame-clock-pacing.md` §8 的兜底定时器驱动。时序类断言（回调延迟、
  p50/p99）在这里量的不是真实节奏。
- **上屏**。没有输出设备就没有"上屏"这件事：toplevel 本身是活的（实测
  `dump: widget 1000x653 css, toplevel 1028x729`，布局与尺寸都拿得到），但
  **popup surface 永远不会被映射**——菜单弹出后 300ms 的回报恒为
  `context menu mapped=0 size=0x0`，真桌面上同一条是 `mapped=1 size=…x…`。
  这是显示器的性质，不是组件的性质，所以 `test/menu-gate.sh` 读
  `CEFNEST_HEADLESS` 把这两条报成 skip（45 条断言照旧全过），别把它当成回归。
- **焦点**。`document.hasFocus()` 在 headless 里恒为 false，真桌面为 true——过与
  不过都一样，所以门禁不依赖它。
- **真实输入设备**。门禁仍然只用脚本步骤驱动 seat，不注入指针；`test/uinput-inject`
  那条要 sudo 的路不在这里跑。

## 6. 实测

`./test/ci-all.sh` 整套跑完 370 秒（T490，九条跑动的和，一次一条命令；2026-10-21
把 proof 的"这一跑做不到它的脚本"扩成四跑之后重测的整表）。"断言"只数门禁自己的
ok 行，不含 harness 那几行记账（socket 起来了 / payload 退出 0 / 没有残留）：

| run | 结果 | 断言 | 用时 |
|---|---|---|---|
| `proof`（无参数自检） | 通过 | 7 | 13s |
| `unit`（`meson test -C builddir`） | 通过 | 14 OK | 16s |
| `pixel` | 通过 | 12 | 12s |
| `find` | 通过 | 34 | 31s |
| `permission` | 通过 | 58 | 60s |
| `download` | 通过 | 46 | 51s |
| `dnd` | 通过 | 59 | 68s |
| `menu` | 通过\* | 45（+2 skip） | 71s |
| `soak` | 通过 | 12 | 48s |

\* 那 2 条是"弹层上了屏"，这块显示器没有屏可上（§5）。`pixel` 在会话里与在真桌面上
都是 12 条全过、0 skip——它是 `docs/internals/frame-clock-pacing.md` §9 那条回归的判据，两侧都得红得起来。

两条补充测量：

- `./test/ci-session.sh ./test/find-gate.sh real` 单跑 = 10 断言 / 26s；
  `bash -c './test/find-gate.sh real && ./test/menu-gate.sh fake'` 同一块显示器 =
  45 断言 + 1 skip / 53s——共享会话这条路径是可用的，只是 `ci-all` 故意不这么跑。
- 在 §8 那个看门狗之前，`find-gate.sh` 这一跑是 4/6，失败的全是 `real-scroll` 的
  `scrollTop was 0 after the step`——不是门禁脆弱，是页面真的冻住了。同一条在真桌面
  上 10 行也照旧全过，而**那不是"看门狗没把能用的一侧改坏"**：§9 之后回看，那一侧
  恰恰已经被改坏了，find 的判据全是日志（命中数、`js` 读回来的滚动值），页面冻在
  空白上时它们一条都不会红。当天能报出来的只有 `pixel-gate`，而它当时被一条 skip
  按住了。

proof 模式那三条断言（`nested compositor on WAYLAND_DISPLAY=`、`HTTP 200`、
`script [0-9]+ ok value="DND-PAGE`）证明的是这块显示器真的能用；那四跑"这一跑做不到它的
脚本"（每跑钉两条：`rc=1`，以及转录里点出原因的那一行）证明的是这台机器上"跑不起来"能以
失败出现、并且说清是哪一种失败，而不是只以超时出现，更不是以 0 出现；`unit` 里 selftest
那 13 个模式本来就在进程内起嵌套 compositor、不依赖外层显示器，那一跑证明的是它们在
会话里也不受干扰。

### 6.1 2026-10-22 复跑：`menu` 这一行不再结束

上表之后工作树动过两处（按名寻址的下载、独立窗口输入的收口），整套重跑了一遍，两件事：

- `unit` 14 → **15**（多了 `seat-selftest-second-window`），`download` 46 → **54** 条 ok
  （`real-named` 那 8 条）。这两条是树的性质，跟显示器无关。
- **`menu` 跑不完**：421 s 被 `ci-all` 的超时砍掉，0 条 ok。卡点是
  `session_menu_show()` 里的 `gtk_popover_popup()`（`src/chromium-session.c:1390`）。
  `WAYLAND_DEBUG=1` 的转录停在这两条之后：

  ```
   -> xdg_surface#55.get_popup(new id xdg_popup#56, xdg_surface#43, xdg_positioner#54)
   -> xdg_positioner#54.destroy()
   -> wl_surface#51.commit()
  ```

  再没有回包。gdb 看主线程：`poll()` ← `wl_display_dispatch_queue()` ← GTK 的
  map/realize ← `gtk_widget_show()` ← `session_menu_show()`——GDK 在等外层那块 headless
  mutter 给这个 popup 发 `xdg_surface.configure`，而它不发。`run_demo` 的 `timeout 90`
  也救不了：进程正阻塞在 roundtrip 里，SIGTERM 没人处理，所以后面几个场景一条都没开始。

  三条反向验证：把这一轮的 `src/nested.c` / `src/nested.h` / `src/chromium-view.c`
  撤掉重编，同一条卡；连 `src/main.c` 一起撤到 HEAD 重编，同一条卡；把同一条脚本放到
  **真桌面**上跑，`menu pick id=132` → `fake: context-menu-select id=132` 一路走完，
  40 s 的看门狗没响。所以这不是这一轮带进来的回归，而是 §5.2 那条"popup 上不了屏"在
  mutter 这边变硬了——以前它至少回一个 configure（否则 300 ms 的
  `context menu mapped=0 size=0x0` 根本报不出来，那个定时器排在 `gtk_popover_popup()`
  后面），现在它连回包都不给。

  **这条没修，也没把门禁降级**。要记住的是它的形状：宿主的主线程阻塞在一次等外层合成器
  的 Wayland roundtrip 上，而同一个进程里正开着嵌套 compositor——外层不回包时，进程内
  没有任何东西还能动。上表 `menu` 那一格（45 + 2 skip / 71 s）在今天的这台机器上复现
  不出来，读它的时候按 2026-10-21 那块显示器理解。

## 7. 换一台机器需要什么

只有第 3 节里"起 compositor"那一段是 GNOME Shell 特有的：`gs.sh` 那一行换成
`cage`、`weston --headless` 或 `gnome-shell --nested` 都行，harness 剩下的部分只
要求"某个进程在 `$XDG_RUNTIME_DIR/$NAME` 上放一块 Wayland socket，并且它是自己进程组
的组长"。本机有 `/dev/dri`，`--headless` 起得来；没有 GPU 的机器上未测。
