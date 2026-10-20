# headless CI 会话：把门禁搬到没人看的显示器上

日期：2026-10-21
问题：P4 需要一个能无人值守跑门禁的地方。本仓库的门禁全都"开真窗口"——在真桌面上
跑等于把窗口糊在使用者脸上，所以它们一直是"文档化的手动门禁"。
结果：`test/ci-session.sh` 起一个 nested mutter（GNOME Shell 48.7 的 `--headless`
模式），把任意命令塞进那个 Wayland 会话里跑。不装包、不用 sudo、现有门禁一行不改，
跑完不留进程。

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
`docs/sandbox-policy.md` §8。

**payload**：`"$@"` 原样在会话里跑。注入的是 `WAYLAND_DISPLAY`、
`GDK_BACKEND=wayland`、隔离的 HOME/XDG/runtime 目录，以及从 compositor 自己的
`/proc/<pid>/environ` 里取出的 bus 地址（这样门户走会话内那条 bus，而不是宿主的），
`CHROMIUM_HELPER` 指向真 helper，超时 `PAYLOAD_TIMEOUT`（默认 900s）。payload 的退出
码就是这一跑的退出码；失败时把 demo/payload 的四个流都 tail 出来。

## 5. 它证明什么，不证明什么

证明：真窗口能在一块真 Wayland 显示器上映射；真 Chromium 会话（`sandbox=on`）能起、
能加载、能回答 JS；`test/*.sh` 那套门禁不需要为 headless 改一行。

不证明：

- **帧节拍**。headless mutter 没有输出设备，宿主 GDK 不再产生显示周期，客户端的帧
  由 `docs/frame-clock-pacing.md` §8 的兜底定时器驱动。时序类断言（回调延迟、
  p50/p99）在这里量的不是真实节奏。
- **焦点**。`document.hasFocus()` 在 headless 里恒为 false，真桌面为 true——过与
  不过都一样，所以门禁不依赖它。
- **真实输入设备**。门禁仍然只用脚本步骤驱动 seat，不注入指针；`test/uinput-inject`
  那条要 sudo 的路不在这里跑。

## 6. 实测

- 自检模式：`ok demo exited 0 against the nested compositor` + 三条 transcript 断言
  + `ok nothing from the nested session is still running`。
- `./test/ci-session.sh ./test/find-gate.sh`：6 个 run / 34 条断言全过，加上 harness
  自己的 3 行正好 37 行 ok。在 §8 那个看门狗之前，这一跑是 4/6，失败的都是
  `real-scroll` 的 `scrollTop was 0 after the step`。
- `./test/ci-session.sh ./test/permission-gate.sh`：10 个 run / 58 条断言全过（加
  harness 的 3 行共 61 行 ok）。
- `./test/ci-session.sh meson test -C builddir`：13/13 OK。selftest 那 11 个模式本来
  就在进程内起嵌套 compositor，不依赖外层显示器，这一跑证明的是它们在会话里也不受
  干扰。
- `./test/find-gate.sh real`（真桌面）：10 行 ok，看门狗没有把能用的一侧改坏。

## 7. 换一台机器需要什么

只有第 3 节里"起 compositor"那一段是 GNOME Shell 特有的：`gs.sh` 那一行换成
`cage`、`weston --headless` 或 `gnome-shell --nested` 都行，harness 剩下的部分只
要求"某个进程在 `$XDG_RUNTIME_DIR/$NAME` 上放一块 Wayland socket，并且它是自己进程组
的组长"。本机有 `/dev/dri`，`--headless` 起得来；没有 GPU 的机器上未测。
