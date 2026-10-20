# 退出路径：关窗口要停 helper，::closed 只发一次

日期：2026-10-20
问题：任务 #27。demo 里 `chromium_session_stop` 只有 header bar 上那颗「停止」按钮
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

## 4. 还没做的

* **compositor 自己的 `wayland-N` + `wayland-N.lock` 仍然留在** `$XDG_RUNTIME_DIR`。
  这个不补：只有 `nested_compositor_dispose` 跑到才会 `wl_display_destroy` → unlink，
  而 GTK 在退进程时并不 finalize widget 树（实测 `close` 路径 view 和 compositor 的
  dispose 都不进）。名字是有界的、会被复用——下一个进程发现 lock 能 flock 成功就
  直接把它抢过来用，所以不会像 uuid 名字那样无限涨。
* `chromium_session_stop` 结尾把 `stopping` 又放回 FALSE，理论上让迟到的
  `child_waited` 有机会把「Operation cancelled」当真错误打出来；目前所有退出路径
  都没复现，先不动。
* 崩在 `kill -9` 下仍会留一个 helper socket——没机会执行任何清理。
