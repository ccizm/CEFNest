# 下载：一个挂住的问题，和四条实测语义

日期：2026-10-21（第四条 2026-10-22）
问题：任务 P2-1（#34–#38）。`docs/guides/control-channel.md` 第 4 节以前把下载判成「不做」，理由
是它不是一问一答：要落盘、要给路径、有进度、能取消。这一篇把形状补出来，并且把 CEF 154
那只有实测才知道的行为钉住——一开始是三条，`real-named` 那一跑撞出了第四条。

## 1. 形状：门禁可以挂着，但只能挂一个

`CefDownloadHandler` 给的两个回调，生命周期不一样，这是整件事的关键：

| 回调 | 什么时候来 | 能不能存下来等宿主回话 |
| --- | --- | --- |
| `CefBeforeDownloadCallback` | `OnBeforeDownload`，**必须返回 true** 才归我们管 | 能。头文件明写它可以异步执行，也可以根本不执行 |
| `CefDownloadItemCallback` | `OnDownloadUpdated`，一次下载来很多次 | 能。`Cancel()`/`Pause()`/`Resume()` 都在它身上 |
| `CefDownloadItem` | 两个回调的参数 | **不能**。头文件要求「不要在方法之外再引用它」，所以跨方法只留标量和上面两个回调 |

于是协议是「一条挂着的问题」而不是「一问一答」：`OnBeforeDownload` 把门禁停在 helper 里、
发一条 `download-begin`、返回 true，然后 CEF 就等。答案什么时候来都行——`download-accept`
带着一个绝对路径过来，`gate->Continue(path, false)`；或者 `download-reject` 把门禁丢掉。
**宿主不答，文件就不落盘**，这是整条路径上唯一的默认值，也是唯一安全的默认值。

`download-begin` 之后 id 就是 Chromium 下载库里的编号。它是 `uint32_t`，**跨会话递增**：
同一次真机跑，前一轮见过 13、这一轮见过 21。宿主不能假定它是 1，所以 demo 面板上的
「保存」和脚本步骤 `download-accept` 都按 id 找自己那行，而不是按序号。

## 2. 谁决定文件落在哪：只有宿主

页面上来的 `suggested_name` 是**建议**，不是路径。demo 面板只用它的最后一个组件：

```c
for (scan = base; *scan; scan++)
  if (*scan == '/' || *scan == '\\')
    base = scan + 1;
```

反斜杠也算，因为这个名字可能是从 Windows 那边的 `Content-Disposition` 抄来的；控制字符换成
`_`；空、`.`、`..` 一律变成 `download`。然后 `CEFNEST_DOWNLOAD_DIR`（或 XDG 的下载目录）
和它拼成绝对路径，同名已经在的话加 ` (1)`、` (2)`。实测的门禁项：

```
load http://gate.test/downloads/fake?name=../../outside.txt
→ demo: download 1 begin name=outside.txt
→ demo: download 1 accept path=/tmp/.../escape/outside.txt
```

`..` 两级没有跑出目录，因为**在拼接之前**就只剩 `outside.txt` 了。宿主给的路径是唯一一条
能把文件写到磁盘上的路——`download-accept` 的空 `path` 会被 helper 拒绝（回一条
`status: download-accept-needs-path`），因为 CEF 把空路径读成「建议名写到默认临时目录」，
那等于凭空造一个宿主不知道在哪的文件。

父目录 helper 和 CEF 都不建：`CEFNEST_DOWNLOAD_DIR` 不存在时是 demo 面板用
`g_mkdir_with_parents` 建的（0700）。

## 3. 进度：按整百分节流，长度未知的另算

`OnDownloadUpdated` 是 Chromium 自己时钟驱动的，一次快速本地下载能进来几十次；而通道是在
**UI 线程**上写的，`SO_SNDTIMEO` 是 200 ms，超时就丢事件（`helper/src/main.cpp` 的
`Control::Send`）。所以只有 `GetPercentComplete()` 变了才发一条，`percent` 字段同时就是节流
键。

但 CEF 的 `GetPercentComplete()` 在总长未知时**永远是 -1**——只用百分数节流的话，一条
chunked 下载从头到尾只发得出**一条**进度事件。所以加了第二条腿：新字节超过
`kProgressByteStep`（256 KiB）也发。

`CefDictionaryValue` 只有 `SetInt(int)`，**没有 64 位 setter**，而 `GetTotalBytes()` /
`GetReceivedBytes()` / `GetCurrentSpeed()` 都是 `int64_t`。字节数一律走 `SetDouble`，宿主用
`json_object_get_double_member` 读回来再转 `gint64`——double 在 2^53 以内精确，2^53 字节
（8 PiB）的文件不是这条通道要商量的事。

## 4. 四条只能实测的语义

这几条都不在头文件里，是真 Chromium 教的，`test/download-gate.sh` 把它们钉住了。

### 4.1 `OnDownloadUpdated` 会先于 `OnBeforeDownload` 到

第一次真机跑的转录：

```
demo: download 1 progress percent=0 received=0 total=6000 speed=0    ← id 宿主还没见过
demo: download 1 begin name=gate.bin mime=application/octet-stream bytes=6000
```

头文件说了「可能在 OnBeforeDownload 前后都来」，但没说宿主会先看到哪一条。答案是**可能
先到进度**。现在的处理是 `struct Download` 上一个 `asked` 标志：`OnBeforeDownload` 设它，
`OnDownloadUpdated` 没见到它就只把 item 回调存下来（那是取消唯一的入口）然后返回。
所以通道上 `download-begin` 永远是那个 id 的第一条，宿主不会为素未谋面的 id 收到进度。

### 4.2 门禁挂着的时候，字节已经在 Chromium 手里了

```
demo: download 13 begin name=gate.bin ... bytes=6000
demo: download 13 progress percent=100 received=6000 total=6000 speed=6000   ← 还没给路径
demo: download 13 accept path=/tmp/.../gate.bin
demo: download 13 end state=complete path=/tmp/.../gate.bin bytes=6000
```

本地服务器 6 KB 一次写完，网络字节全进了 Chromium 的缓冲区，`received_bytes` 就是那个数——
而磁盘上这时候一个字节都没有。**进度说的是下载收到了多少，不是文件写了多少**，两者在
helper 这里没有分开的手段（`CefDownloadItem` 不给「已写盘字节」）。宿主 UI 别把 percent=100
念成「文件已经在磁盘上了」：`complete` 才是。

### 4.3 取消之后 `path` 和 `received_bytes` 都是空的

```
demo: download 21 begin name=stream.bin ... bytes=2097152
demo: download 21 progress percent=48 received=1015808 total=2097152
demo: download 21 reject
demo: download 21 end state=cancelled path= bytes=0
```

`Cancel()` 之后 CEF 的终止更新里 `IsCanceled()` 为真，而 `GetFullPath()` 和
`GetReceivedBytes()` 都归零了——宿主**不能**从 `download-end` 知道取消前到了多少字节，
那个数只在最后一条 `download-progress` 里。目录实测是空的：Chromium 把半截文件删了，宿主
不需要自己扫。

被拒绝、根本没开始的下载走另一条路：`OnDownloadUpdated` 不会为它来（CEF 没启动那次下载），
所以终止的 `download-end` 由 helper 自己补一条 `state=cancelled`、`path=""`、
`received_bytes=0`——和 4.3 的形状**故意一致**，宿主分不出这两种「没有」，也不需要分：
两条都表示没有文件。一次真机拒绝实测到恰好一条 `end state=cancelled`。

### 4.4 门禁挂久了再给路径，`complete` 会晚 3–6 s 才来

`real-named` 第一次跑就把这条撞了出来。转录（时间戳是门禁给每行盖的）：

```
00:46:13.958 demo: download 136 accept path=/tmp/.../named/second.bin
00:46:13.961 demo: download 135 end state=cancelled path= bytes=0     ← 另一条按名砍掉
00:46:16.664 demo: download 136 end state=complete path=/tmp/.../named/second.bin
```

`second.bin` 的 mtime 是 `00:46:13.956`——**文件在给路径的那一刻就落盘了**，`end` 却在
2.7 s 之后才到。连着三次实测 `accept`→`end state=complete` 的间隔是 **3.24 / 4.12 / 5.72 s**。
字节早在 4.2 节说的那条 `percent=100` 里就到了，`Continue()` 只是把它挪进最终路径，
可 CEF 的终止更新不是跟着这次挪动来的，它和 Chromium 下载库的批量落盘一起排。
另一种情形能证明它是「排着」而不是「丢了」：同一条 second.bin，在另一条 stream 自然跑完
的时候，`end` 就紧跟在那条 stream 的最后一条 progress 后面出来——有活动把它冲出来了。

宿主代码这里没有任何补救可做：`CefDownloadItem` 不给「已写盘」这个状态，helper 也不该拿
`percent=100` 冒充 `complete`。能做的是把语义写清楚（4.2 已经写过一次，这条是它的加强版：
`complete` 不只是「写盘」，还会晚几秒），以及让门禁等够——`real-named` 的收尾等待从 4 s
改成 9 s，因为 4 s 真的会只看到文件、看不到事件。

## 5. 策略的默认值：demo 面板做了什么

`src/main.c` 的下载区只做两件事——把 id 变成一行 UI，和决定要不要立刻给路径：

| 环境变量 | 行为 |
| --- | --- |
| （都不设） | 自动接受，写到 XDG 下载目录（拿不到就 `$HOME`） |
| `CEFNEST_DOWNLOAD_DIR=<绝对路径>` | 同上，但写到这个目录；不存在就建 |
| `CEFNEST_DOWNLOAD_ASK=1` | 谁都不自动接受，行停在那里等「保存」/「丢弃」，等脚本步骤 `download-accept <path>` / `download-reject`（按名寻址的变体见 §脚本步骤） |

两个都可以设：`ASK=1` 赢，目录只在人真的点了保存时用来算一个默认名。策略是在启动时打印
出来的，转录里能看到，门禁就靠这行确认自己跑的是哪条政策：

```
demo: downloads: dir=/tmp/dlprobe/refuse ask=1
```

`download-accept` / `download-reject` 这两条脚本步骤挑的是**面板里第一个还没决定的行**，
不是某个 id：一个静态脚本没法预先知道 Chromium 的编号（§1）。`reject` 挑第一个还没结束的
行，所以它既能拒未开始的、也能砍在飞的，和协议里那条命令一致。

按位置挑在两个下载同时活着的时候说不清指谁。`download-accept-name <子串> <path>` 和
`download-reject-name <子串>` 按**建议名或 URL 的子串**挑行——脚本知道自己向页面要的是
哪个文件，不知道 Chromium 的编号。门禁场景 `real-named`（§6）同时挂一个在飞的 stream.bin
和一个停着的 second.bin：按名接受 second 到自己的路径、按名砍掉 stream，断言目录里只有
second.bin、且两种 end 各恰好一条。

## 6. 门禁

`test/download-gate.sh`（`fake` / `real` / 两者）十一次运行、50 条断言：

| 场景 | 钉住的东西 |
| --- | --- |
| `fake-auto` | 自动落盘、进度、`complete`、同名变成 `auto (1).txt` |
| `fake-escape` | `../../outside.txt` 只剩 `outside.txt`，目录外没有文件 |
| `fake-ask` | `ASK=1` 真的不动；脚本给的路径（含父目录）被创建并写对字节数 |
| `fake-refuse` | 拒绝未开始：一条 `cancelled`，目录空 |
| `fake-cancel` | 在飞时取消：有 `percent`，`cancelled`，半截文件没了 |
| `fake-never` | 根本不答：没有 accept/reject，也没有任何 `download-end` |
| `real-auto` | 真 `Content-Disposition: attachment` → `download-begin`，6000 字节落盘 |
| `real-refuse` | 真机拒绝：恰好一条 `cancelled`，目录空 |
| `real-cancel` | 真机在飞取消：`percent` 在 1–99 之间，`cancelled` |
| `real-named` | 两个下载同时活着时按名寻址：second 落自己的路径、stream 被砍且不留文件 |
| `real-never` | 真机不答：目录空（进程退出时门禁自己烂掉，没有文件） |

`real-*` 那五次要一个真的 HTTP 服务器，脚本自己起了一个临时线程服务器：`/blob` 一次写完
6000 字节，`/blob2` 4321 字节是给 `real-named` 的第二个附件，`/stream` 2 MiB 按 32 KiB /
100 ms 往外滴。**滴是必要的**——本地地址上一次不加控制的下载，在宿主有机会取消之前就已经
`complete` 了，第一版门禁就是这么撞上「no download left to reject」的。`real-named` 的收尾
等待写死 9 s，因为 4.4 那条 `complete` 的迟到：4 s 的时候文件在、事件还没来。

它开真窗口、跑真浏览器，所以和 `test/*.session` 同一档：手跑门禁，不进 `meson test`。
`meson test -C builddir` 15/15 与 `python3 test/audit-impl-slots.py builddir` 不受影响。


## 暂停 / 恢复（2026-10-21）

`CefDownloadItemCallback` 的 `Pause()` / `Resume()` 已上通道：

- host → helper：`download-pause`、`download-resume`，载荷 `id`；
- helper 只在已经拿到 `CefDownloadItemCallback`（下载确实在飞）时调用；还没开始的下载
  回 `status: download-not-in-flight`；
- 终态仍由 CEF 的 `OnDownloadUpdated` 发 `download-end`，pause 不伪造结束事件；
- API：`chromium_session_pause_download()` / `chromium_session_resume_download()`；
- demo 脚本：`download-pause` / `download-resume`，作用于第一条未完成的下载。

进度事件本身没有额外的 `paused` 字段；宿主知道自己发过 pause，UI 可以根据调用状态显示。
