# 控制通道：现在的命令、事件，和决定不加的那些

日期：2026-10-20
问题：任务 #25。`load-error` 到了通道里只进 `g_warning`，宿主没有任何办法知道"这次没加载
成功"；renderer 死了更是完全没有出口。这一篇把这两条补成信号，并且把"还要不要再加命令"
这件事一次性定下来。

> 2026-10-21 更新（任务 #34–#38）：下载做了。它是 §4 那张表里唯一一条**因为协议形状不够**
> 而被判"不做"的功能，这次把形状补上了，所以 §3、§4、§5 都跟着改。细节在
> `docs/download-path.md`，这一篇只讲通道。

## 1. 缺的那一条为什么必须是一条信号

`::load-end` 在失败的导航上**也会发**，HTTP 码是 0。真 Chromium 的两次失败（DNS 解析不了
和端口被 Chromium 自己禁掉）都是这个形状：

```
demo: uri=https://example.com/ title=Example Domain loading=1     ← 新导航开始
helper: load error -105 for https://no-such-host.invalid/
demo: load failed -105 https://no-such-host.invalid/ ERR_NAME_NOT_RESOLVED   ← 新
demo: uri=https://example.com/ title=no-such-host.invalid loading=1
demo: loaded https://no-such-host.invalid/ (HTTP 0)                ← 同一件事的 load-end
demo: uri=https://example.com/ title=no-such-host.invalid loading=0
```

只听 `::load-end` 的宿主会读到 `HTTP 0`——那不是"加载成功，状态码 0"，但除了这条信号没有
别处说清它。还有一点要记住：**`uri` 不会跟着失败变**（`OnAddressChange` 只在真正提交的导航
上发），所以上面第三行里 `uri=` 还是旧页。

另外，一次失败会报**两次**：真实错误码先来，约一秒后跟着一条 `-3 ERR_ABORTED`。原样传出去，
不筛——筛掉 -3 就等于替宿主决定"哪个错误值得看"，而那些错误在别的形状下（用户中途点了别的
链接）是真要看的。

```
WARNING: session: load error -3 for https://no-such-host.invalid/: ERR_ABORTED
```

## 2. renderer 死了：不报就是"页面卡住不动"

`CefRequestHandler::OnRenderProcessTerminated`（CEF 154 里它在 request handler 上，不在
life-span handler 上，签名是 `(browser, status, error_code, error_string)`）。以前没实现。
现在发一条事件、转成一条信号：

```
helper: render process terminated (status=1 error=9)
demo: renderer exited status=1 error=9 9
```

`status` 用 CEF 的 `cef_termination_status_t` 数值：0 非零退出、1 被 SIGKILL、2 段错误、
3 OOM（往后两个是 Windows/启动失败，这条路上碰不到）。上面这次是 `kill -9`，所以
`status=1`、`error_code=9`；`error_string` 这次就是信号号的文本，别指望它是句子。

关键在于**除此之外什么都看不到**：那条信号之后，转录里一个 `demo:` 行也没有了——表面还停在
最后一帧，`js` 发出去不再回答，`changed` 也不再动。这就是第 22 号任务结尾记下的那个洞
（`docs/sandbox-policy.md` 第 7 节）：沙箱开起来之后，renderer 因为 seccomp 被杀只表现为
"页面静止"。

**复现的办法**（也是这次量的办法）：跑一份 `wait 30000` 的会话，找 `/proc` 里 `Seccomp: 2`
的 `cefnest-helper`，`VmRSS` 最大那一个就是 renderer，`kill -9` 它。

一个警告：`Seccomp=2` 的进程**不止 renderer**。第一次试着杀了 pid 最小的那个，宿主一条事件
都没收到——CEF 只为渲染进程发这个回调。所以这份信号说的是"这一页的渲染进程没了"，不是
"helper 掉了什么"。

## 3. 现在通道上有的东西

命令 10 条，事件 14 条（`helper/src/main.cpp` 的 `HandleCommand` / `NewEvent`）：

| 方向 | 名字 | 载荷 |
| --- | --- | --- |
| host → helper | `load` | `url` |
| host → helper | `back` `forward` `reload` `stop` `quit` | `reload` 多一个 `hard` |
| host → helper | `js` | `code`、`id` |
| host → helper | `mute` | `on`（缺省 = 翻转） |
| host → helper | `download-accept` | `id`、`path`（绝对路径，父目录得宿主自己建） |
| host → helper | `download-reject` | `id`（拒绝未开始的、取消在飞的，同一条） |
| helper → host | `ready` | `cef`、`pid`、`url` |
| helper → host | `address` | `url`（只有真正提交的导航才发） |
| helper → host | `title` | `title` |
| helper → host | `loading` | `loading`、`can_go_back`、`can_go_forward` |
| helper → host | `load-end` | `url`、`http_code`（**失败也发，码为 0**） |
| helper → host | `load-error` | `error_code`、`url`、`message` |
| helper → host | `renderer-exited` | `status`、`error_code`、`message` |
| helper → host | `muted` | `muted`（`SetAudioMuted` 之后的 `IsAudioMuted()` 回读） |
| helper → host | `js-result` | `id`、`ok`、`value`、`message` |
| helper → host | `download-begin` | `id`、`url`、`suggested_name`、`mime_type`、`total_bytes` |
| helper → host | `download-progress` | `id`、`received_bytes`、`total_bytes`、`percent`、`speed` |
| helper → host | `download-end` | `id`、`state`（`complete`/`cancelled`/`interrupted`）、`path`、`received_bytes` |
| helper → host | `status` `closed` | 文本 / 无 |

字节数一律走 JSON 的 double：`CefDictionaryValue` **只有 `SetInt(int)`，没有 64 位 setter**，
而 `GetTotalBytes()` 他们就是 `int64_t`。double 在 2^53 以内是精确的，这条通道上的文件不可能
到那个量级。

`download-*` 这三条事件不是一个问答，而是一串：`id` 是 Chromium 下载库里的编号（**跨会话
递增**，实测一次跑下来见到 13、21），宿主用同一个 `id` 回答。形状与判据的冲突怎么解决、
进度为什么按整百分节流、取消之后 `path` 为什么是空的，都写在 `docs/download-path.md`。

`ChromiumSession` 的信号：`::changed`、`::ready`、`::load-end`、`::load-error`、
`::renderer-exited`、`::script-result`、`::download-begin`、`::download-progress`、
`::download-end`（最后三条新）、`::closed`。`::changed` 现在也会随 `muted` 事件落地。

`load-error` 和 `renderer-exited` 都是 3 个参数，`renderer-exited` 那两个 int 一个是 CEF 的
枚举、一个是平台错误码——刻意不合并成一条"出错了"信号：一个是"这一页没来"，另一个是
"这一页没了"，宿主对两者的处理不一样（前者换个地址，后者重载或者报进程问题）。

## 4. 决定：剩下的命令先不加

逐条说清为什么现在不做，而不是留一个"TODO"。判断标准是同一个：**这条命令能不能只用一次
一问一答表达**。NDJSON 通道一问一答很稳，凡是"有进度、要回调、得落地到磁盘"的，都要先在协议
上想清楚形状。

| 想要的功能 | CEF 那边 | 决定 | 为什么 |
| --- | --- | --- | --- |
| 页内查找 | `CefBrowserHost::Find` + `CefFindHandler`（命中数、高亮位置都是回调） | **不做**，先记着 | 一问一答装不下"总共 N 处，当前第 M 处"；要做得先给通道加相关任务的 id 空间，和 `js-result` 那套是两码事 |
| 页面缩放 | `SetZoomLevel` / `GetZoomLevel` | **不做** | 代价最小的一条，但宿主此刻没有理由需要它：控件尺寸变了 Chromium 自己会重排。等真出现"字号设置"的产品需求再加 |
| 静音 / 音量 | `SetAudioMuted` / `IsAudioMuted` | **mute 已做**，音量不做 | `docs/audio-path.md` 把路径量掉了：页面出的声住在一个不沙箱的 `audio.mojom.AudioService` utility 里、直达宿主桌面，组件自己的开关只有这一个；它一次请求、无需回答，符合判据。音量是桌面混音器已经管着的事，不再画第二遍 |
| 下载 | `CefDownloadHandler::OnBeforeDownload` + 进度 + 取消 | **已做**（2026-10-21） | 当初判"不做"的理由是对的：它不是一问一答。解法不是硬塞进一问一答，而是给通道加一个**挂着不答的问答**——`OnBeforeDownload` 把 CEF 的门禁回调停在 helper 里，发一条 `download-begin` 就返回，答案由宿主在方便的时候用 `download-accept`/`download-reject` 送回来。宿主不答，文件就不落盘。细节见 `docs/download-path.md` |
| DevTools | `ShowDevTools` | **不做** | 那是第二个顶层窗口；嵌套合成器现在只合成一个 toplevel + 它的 popup（#21），要开就是 `xdg_toplevel` 的第二实例，属于协议侧的决定 |
| 历史列表 | `CefNavigationEntryVisitor` | **不做** | 现在只有 `can_go_back/forward` 两个布尔；列表是查询型（要 id 空间），同"页内查找" |
| 清缓存 / 清 cookie | `ClearCache`、`CefCookieManager` | **不做** | 得先定义三档（缓存 / cookie / localStorage）分别意味着什么，这是隐私策略不是 API |
| 打印 / 存 PDF | `PrintToPdf` | **不做** | 形状和下载是同一套（挂住的回调 + `id` + 落盘路径），照抄就行；但 `docs/download-path.md` 那三条实测还没给它，不做没有证据的功能 |
| 自定义 UA | `CefRequestContext` | **不做** | 走环境变量比走命令合适：它是一次性设定，不该中途变 |

反过来，**这 10 条够用**的证据还是这套组件的全部交互：导航（`load`/`back`/`forward`/
`reload`/`stop`）、任意脚本（`js` 一条顶十条，页内状态、标题、当前 URL 都能自己读）、退出
（`quit`）、加上现在这条有进度要回话的（`download-*`）。要加的新命令得先说明它不能用 `js`
表达——上面那张表里除了 DevTools，其余几条其实都能用 `js` 大致凑出来，只是凑得不干净；
下载**不能**凑，因为它落盘、要宿主给路径，而 `js` 那条通道一个字一个字都在页面里。

## 5. 验证

信号那两条都两头看过：

* **假 helper**（`test/fake-helper.py`，不启动浏览器）。它现在按 §1 的形状回答 `.invalid`
  地址，并按 `FAKE_HELPER_CRASH_AFTER` 报一次 renderer 死亡：

  ```sh
  GDK_BACKEND=wayland CHROMIUM_HELPER=$PWD/test/fake-helper.py \
    CHROMIUM_SESSION_SCRIPT=$PWD/test/error.session builddir/src/cefnest-demo
  # demo: load failed -105 https://no-such-host.invalid/ ERR_NAME_NOT_RESOLVED
  # demo: loaded https://no-such-host.invalid/ (HTTP 0)
  ```

  `test/error.session` 这个脚本真假两边都能跑：同一个 `.invalid` 地址，真 Chromium 给的是
  同一条 `ERR_NAME_NOT_RESOLVED`。

* **真 Chromium**：`kill -9` 那三个 `Seccomp=2` 里 VmRSS 最大的一个，转录里就是 §2 那两行；
  会话随后 `quit`，`helper stopped (stopped by the host)` 照常——renderer 死了不影响退出
  路径（#27 那三条路仍然成立）。

`mute` / `muted` 那一对也两头看过：假 helper 回读状态，真 Chromium 用 `docs/audio-path.md`
第 3 节的空 sink 录音证明 mute 之后到达音频服务器的是精确的数字静音。

`download-*` 五条是**先量再做**的：`test/download-gate.sh` 十次运行（六次假 helper、
四次真 Chromium）、41 条断言，把自动落盘、改名去重、`../../` 逃不出去、拒绝未开始、
取消在飞、以及"根本不答所以什么都没有"六条路都钉住；实测到的三条 CEF 行为写在
`docs/download-path.md` §4。它开真窗口，所以和 `test/*.session` 一样是手跑门禁，
不进 `meson test`。

`meson test -C builddir` 10/10 照旧全过（那套测的是 compositor 侧，不碰控制通道）。

## 6. 还没做的

* **一条命令都没有应答机制**。`js` 有 `id`，其余命令发出去就完了；宿主不知道 `load` 有没有
  被吃到（`address`/`load-end` 是间接证据）。要不要给每条命令配一个 `ok`，等 §4 里出现
  需要它的那条再说。
* **utility 进程（网络、音频）掉了没人报**。CEF 只为渲染进程发这个回调，§2 实测过杀另一个
  是没有声音的。音频现在确认就住在一个 `Seccomp=0` 的 `audio.mojom.AudioService` utility
  里（`docs/audio-path.md`），宿主能看到的兜底只有 `::closed`。
* `::renderer-exited` 之后 helper **不会自己把页面重载**，也不该——重载策略是宿主的。
* **下载没有暂停 / 续传**。`CefDownloadItemCallback` 上就摆着 `Pause()` 和 `Resume()`，
  协议里没有对应的两条：一个 `pause` 不需要宿主再给任何信息，看起来该做；但demo 面板上
  一个按钮和一条命令都是多余的，直到真有宿主提这个要求。
* **`interrupted` 这一支没有被实测覆盖**。门禁能造出 `complete` 和 `cancelled`（服务端
  半路断连才行），断网、证书错误、磁盘写满是同一支的三个来源，`test/download-gate.sh`
  现在一个都不制造。事件形状是好的（`state=interrupted`、`path` 由 CEF 决定），只是
  还没有证据说它在真机上一定按这个形状到。
