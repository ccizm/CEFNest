# 控制通道：现在的命令、事件，和决定不加的那些

日期：2026-10-20
问题：任务 #25。`load-error` 到了通道里只进 `g_warning`，宿主没有任何办法知道"这次没加载
成功"；renderer 死了更是完全没有出口。这一篇把这两条补成信号，并且把"还要不要再加命令"
这件事一次性定下来。

> 2026-10-21 更新（任务 #34–#38）：下载做了。它是 §4 那张表里唯一一条**因为协议形状不够**
> 而被判"不做"的功能，这次把形状补上了，所以 §3、§4、§5 都跟着改。细节在
> `docs/download-path.md`，这一篇只讲通道。
>
> 2026-10-21 更新（任务 #40–#45）：页内查找也做了。它当初被判"不做"的理由同样成立——命中数
> 和当前第几处都是回调，不是一问一答——解法也和下载同一类：**命令不发回答，回答是一条事件**。
> §3、§4、§5、§6 又跟着改了一遍，实测到的四条语义在 `docs/find-path.md`。
>
> 2026-10-21 更新（任务 #56）：上下文菜单也做了。它是第三种形状：前两条要补的是"回答装不进
> 一问一答"，这一条要补的是**一个必须由宿主作答、否则右键就再也问不出第二个问题**的请求。
> `RunContextMenu` 把 CEF 的模型交给宿主画，答案必须回来。实测到的那条硬约束（helper 那边
> 同一时刻只持有**一个**没答的菜单）写在 `docs/context-menu.md` §2。
>
> 2026-10-21 更新（任务 #57–#62）：权限提示也做了。它是**第四种**形状，和菜单同族但更狠：菜单不答是
> 右键坏掉，权限不答是页面的 promise 挂住、**而且页面紧接着的下一次问法会凭空消失**（实测两次，
> `docs/permission-prompt.md` §3）。它还带来一个菜单没有的结构问题：media 和 prompt 是**两条互不相干的
> 队列、各自一个槽**，所以 session 这边握的是一张表而不是一个布尔。§3、§4、§5、§6 第四次跟着改。
>
> 2026-10-22 更新（M10、M14）：**读这一侧也立了规矩**。以前只有写侧有（S4 补的部分写循环）。这条通道
> 对"对端是另一个版本"的态度一直是"整条打出来看看"——那等于承认了事件的**名字**可以不同，却没有承认
> **字段可以缺席**。现在缺字段有各自类型的答法，一条没有换行的流有上限。写在"通道读这一侧"那一节。

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

命令 25 条，事件 26 条（`helper/src/main.cpp` 的 `HandleCommand` / `NewEvent`）：

| 方向 | 名字 | 载荷 |
| --- | --- | --- |
| host → helper | `new-browser` | `url`、`mode`（`tab`/`window`；同一 helper 里的新 CEF browser） |
| host → helper | `select-browser` | `browser`（标签激活；窗口模式也会 focus 那个 CefWindow） |
| host → helper | `close-browser` | `browser`（只关这个 browser；最后一个关掉 helper） |
| host → helper | `load` | `url`，可带 `browser`；缺省 = 当前激活 browser |
| host → helper | `back` `forward` `reload` `stop` `quit` | `reload` 多一个 `hard` |
| host → helper | `zoom-in` `zoom-out` `zoom-reset` | 无（CEF zoom level 步进 0.5；reset 回 0.0） |
| host → helper | `js` | `code`、`id`（语句形状；页面侧 `eval`，受 CSP `unsafe-eval` 限制） |
| host → helper | `js-expr` | `code`、`id`（表达式直接嵌入，不经过 `eval`；CSP 严格页面用这条） |
| host → helper | `mute` | `on`（缺省 = 翻转） |
| host → helper | `a11y` | `enabled`（`SetAccessibilityState(STATE_ENABLED)` / `STATE_DEFAULT`；宿主默认由 `org.a11y.Status.ScreenReaderEnabled` 驱动） |
| host → helper | `download-accept` | `id`、`path`（绝对路径，父目录得宿主自己建） |
| host → helper | `download-reject` | `id`（拒绝未开始的、取消在飞的，同一条） |
| host → helper | `download-pause` | `id`（只对已拿到 item callback 的下载有效；未开始回 status） |
| host → helper | `download-resume` | `id`（同上） |
| host → helper | `find` | `pattern`、`forward`、`match_case`、`next`（**没有 ack**，答案是 `find-result`） |
| host → helper | `find-stop` | `clear`（缺省 true：连高亮一起撤） |
| host → helper | `context-menu-select` | `id`、`flags`（可选，API 上没开）（**`context-menu` 必须被答**：helper 同时只持有一个请求，不答就再也问不出第二个） |
| host → helper | `context-menu-dismiss` | 无（`Cancel()`：关掉而不执行任何命令，这条回答的意思是"什么也没选"） |
| host → helper | `permission-respond` | `id`、`result`（`accept`/`deny`/`dismiss`；**`permission-request` 必须被答**，而且 media 那边的 `accept` 只能是"整份给"——`Continue()` 要求 `allowed_permissions` 等于请求里的 `required`） |
| helper → host | `ready` | `cef`、`pid`、`url`（第一个 browser 的握手） |
| helper → host | `browser-created` | `browser`、`url`、`windowed` |
| helper → host | `browser-selected` | `browser`、`windowed` |
| helper → host | `browser-closed` | `browser` |
| helper → host | `address` | `url`（只有真正提交的导航才发） |
| helper → host | `title` | `title` |
| helper → host | `loading` | `loading`、`can_go_back`、`can_go_forward` |
| helper → host | `load-end` | `url`、`http_code`（**失败也发，码为 0**） |
| helper → host | `load-error` | `error_code`、`url`、`message` |
| helper → host | `renderer-exited` | `status`、`error_code`、`message` |
| helper → host | `muted` | `muted`（`SetAudioMuted` 之后的 `IsAudioMuted()` 回读） |
| helper → host | `zoom` | `level`（`SetZoomLevel` 之后的 `GetZoomLevel()` 回读；每次 zoom 命令都发，主 frame `load-end` 后也发一次，因为 Chromium 按 origin 持久化；0.0 = 100 %） |
| helper → host | `copy-text` | `text`（菜单里的“复制链接地址/复制图片地址/复制媒体地址”；宿主写系统剪贴板） |
| helper → host | `open-url` | `url`、`disposition`（菜单里的“在新窗口打开…”；demo 起第二个窗口，库宿主自己决定） |
| helper → host | `page-message` | `json`（页面 `cefnestPostMessage` 的原始字符串；schema 归宿主） |
| helper → host | `primary-selection` | `text`（页面选区经注入的 listener 推给宿主；宿主写 Gtk primary，空串表示撤销选区） |
| helper → host | `js-result` | `id`、`ok`、`value`、`message` |
| helper → host | `download-begin` | `id`、`url`、`suggested_name`、`mime_type`、`total_bytes` |
| helper → host | `download-progress` | `id`、`received_bytes`、`total_bytes`、`percent`、`speed` |
| helper → host | `download-end` | `id`、`state`（`complete`/`cancelled`/`interrupted`）、`path`、`received_bytes` |
| helper → host | `find-result` | `identifier`、`count`、`active_ordinal`、`final`（没有命中矩形，理由见 `docs/find-path.md` §2.4） |
| helper → host | `context-menu` | `x`、`y`、`items`（每项 `id`、`label`、`type`、`enabled`、`checked`；只发 `IsVisibleAt` 为真的行） |
| helper → host | `context-menu-dismissed` | 无（**只在 helper 还握着那个回调时发**：宿主自己答过之后 CEF 关菜单也算 dismiss，那一条被滤掉了） |
| helper → host | `permission-request` | `id`、`kind`（`media`/`prompt`）、`origin`、`permissions[]`、`raw`（mask 原值；两套枚举只存在于 `include/internal/cef_types.h`，公共头文件一个都不列） |
| helper → host | `permission-dismissed` | `id`、`result`（**只有 prompt 有这条**：`CefMediaAccessCallback` 上没有"宿主没答、被浏览器自己关掉"的回调，media 只能一直握着） |
| helper → host | `status` `closed` | 文本 / 无 |

所有带页面的 helper → host 事件都带 `browser`（当前激活 browser 的 CEF id）；`new-browser`
创建的 browser 在页面加载前先发 `browser-created`，所以 `url` 是请求的目标地址。现有命令
里的 `browser` 字段是可选的，缺省仍是“当前激活 browser”，这让旧的单 browser 调用保持
兼容。

字节数一律走 JSON 的 double：`CefDictionaryValue` **只有 `SetInt(int)`，没有 64 位 setter**，
而 `GetTotalBytes()` 他们就是 `int64_t`。double 在 2^53 以内是精确的，这条通道上的文件不可能
到那个量级。

`download-*` 这三条事件不是一个问答，而是一串：`id` 是 Chromium 下载库里的编号（**跨会话
递增**，实测一次跑下来见到 13、21），宿主用同一个 `id` 回答。形状与判据的冲突怎么解决、
进度为什么按整百分节流、取消之后 `path` 为什么是空的，都写在 `docs/download-path.md`。

`find` / `find-result` 是同一类问题的第二种解法：`find` **连挂着的问题都不发**，发出去就完，
命中数全部从 `find-result` 回来。一次请求可以答不止一条（还在数的那几条 `final=false`），
`identifier` 实测**每次请求 +1** 而不是每次搜索 +1，起点还是 0——它只用来把一次请求的多条
回答归到一起。四种只可实测的行为写在 `docs/find-path.md` §2。

`context-menu` 是第三种：它**必须**被回答，而且回答它的那两条命令不带任何回答。前两条要补的
是"回答装不进一问一答"，这一条要补的是"这个问题答不了"——CEF 的 `CefRunContextMenuCallback`
是宿主拿菜单的唯一途径，而 `docs/context-menu.md` §2 量到 Chromium 那边**同时只允许一个没答的
请求**：连点五次右键，只有第一次把菜单送到通道上，后四次全被页面自己的 `contextmenu` 吃掉，
直到宿主回了那一个。所以"不答"在这里不是安静，是把右键弄坏了。定下的规则是**会话永远会答**：
宿主不接管就画 CEF 给的默认菜单，宿主接管了则由宿主的 `select`/`dismiss` 作答，宿主既没接管
又答不了（没有 view、或模型里一条命令都没有）时会话自己 `dismiss`。

`permission-request` 是第四种，形状和菜单一样（一条必须回答的问题 + 一条不带回答的答案），代价却大得
多。菜单不答是"下一次右键安静地没了"；权限不答是**页面站在答案上**：Alloy 的默认处理是 media deny、
prompt IGNORE，而 IGNORE 传到你手上就是一条永远不 settle 的 promise（`Notification.requestPermission()`、
`clipboard.readText()`、`requestMIDIAccess()` 三个在基线里全是 `pending`）。更实测的是队列：media 一次
只走一条，扣着的那条没答，后面的 media 问法**根本不到宿主**，答掉之后才作为它自己的新 id 到达；prompt
之间会互相顶掉（旧的一条在新的到达**之前**先被 dismiss 成 `ignore`）；两条队列互不影响。还有一条没有
解释但复现了两次的：**面板扣着一条 prompt 时，页面紧接着的下一次问法会凭空消失**——不是排队，因为
它再也没有出现过。所以规则仍然是"会话永远会答"，只是这里的答案只有一个方向：宿主不接管就 deny。
`id` 是 media 和 prompt 共用的一个递增序号，槽位则是各自的，所以 session 握的是一张
`GHashTable`（`permission_held`），不是一个布尔。细节和全部转录在 `docs/permission-prompt.md`。

`ChromiumSession` 的信号：`::changed`、`::ready`、`::load-end`、`::load-error`、
`::renderer-exited`、`::script-result`、`::download-begin`、`::download-progress`、
`::download-end`、`::find-result`、`::context-menu`、`::permission-request`、
`::permission-dismissed`（最后三条新）、`::closed`（带一个原因字符串：`stopped by the
host` / `helper closed the browser` / `control channel closed` / `process exited`，
实测与取舍在 `docs/session-lifecycle.md` §5）。多浏览器那三条 `::browser-created`
（id, url, windowed）、`::browser-selected`（id）、`::browser-closed`（id，tab 上
永远不来——`close-browser` 的三方实测在 `docs/multi-browser.md` §1）和页面侧两条
`::page-message`（json）、`::popup-requested`（url）在 README 的信号段和
`src/chromium-session.h` 里各有一份带形状的说明。`::changed`
现在也会随 `muted` 事件落地。

`::context-menu` 和 `::permission-request` 是这套信号里**唯二有返回值的**：`gboolean`，handler 返回 TRUE 表示"这个问题我接了，答案我来负责"，返回 FALSE 表示"交给会话"（菜单→画 CEF 给的默认菜单，权限→直接 deny）。答案取的是 handler 的返回值
——GType 为 `G_TYPE_BOOLEAN` 且没注册累加器的信号，累加的就是最后一个 handler 的返回值，
`g_signal_emit` 那个 `&handled` 位置**不会**传进 handler（它声明成出参写进去就是段错误，
`docs/context-menu.md` §7 记了这次的现场）。`::permission-request` 用的是同一条规则，第一版 demo 差点
把同一个坑再踩一遍。

`load-error` 和 `renderer-exited` 都是 3 个参数，`renderer-exited` 那两个 int 一个是 CEF 的
枚举、一个是平台错误码——刻意不合并成一条"出错了"信号：一个是"这一页没来"，另一个是
"这一页没了"，宿主对两者的处理不一样（前者换个地址，后者重载或者报进程问题）。

### helper 进程拿到的环境

自写 helper 的人在说话之前得先满足这些——宿主在起子进程时替它设好的
（`src/chromium-session.c:1999-2005`，`GSubprocessLauncher`）：

| 变量 | 值 | 为什么 |
| --- | --- | --- |
| `WAYLAND_DISPLAY` | 这个 `ChromiumView` 自己的 compositor socket 名 | helper 的 CEF 直接连到嵌套 compositor，页面才有地方上屏 |
| `GDK_BACKEND` | `wayland` | 不许 GTK 在 helper 里挑别的后端 |
| `XDG_SESSION_TYPE` | `wayland` | 同上，Chromium 自己也读这个 |
| `CHROMIUM_HELPER_SOCKET` | 控制通道 socket 的全路径 | 这条文档说的 NDJSON 就走它 |

同时被**撤掉**的：`DISPLAY`（helper 只许走 Wayland，掉回 X11 是路线 B 明令禁止的）
和 `WAYLAND_SOCKET`（防止外面的 fd 被当成自己的 compositor）。别的变量是谁起
helper 谁管：demo 会往里放 `CHROMIUM_URL`、顺手清掉自己那几个脚本/转储变量
（`src/main.c:525-529`），但那不是这条通道的握手，自写宿主不需要模仿。

## 4. 决定：剩下的命令先不加

逐条说清为什么现在不做，而不是留一个"TODO"。判断标准是同一个：**这条命令能不能只用一次
一问一答表达**。NDJSON 通道一问一答很稳，凡是"有进度、要回调、得落地到磁盘"的，都要先在协议
上想清楚形状。

| 想要的功能 | CEF 那边 | 决定 | 为什么 |
| --- | --- | --- | --- |
| 页内查找 | `CefBrowserHost::Find` + `CefFindHandler`（命中数、高亮位置都是回调） | **已做**（2026-10-21） | 当初的理由仍然对：一问一答装不下"总共 N 处、当前第 M 处"。解法是不硬塞——`find` 不发回答，命中情况全部走 `find-result` 事件，一次请求可以来好几条（`final` 才是答案到了）。宿主侧要的状态只有"数到几、第几"，`selectionRect` 不上线：`docs/find-path.md` §2.4 量到浏览器自己会把**选中的**那条滚进视野，而宿主画不进页面 |
| 页面缩放 | `SetZoomLevel` / `GetZoomLevel` | **已做**（2026-10-21） | 三条无回答命令（`zoom-in`/`zoom-out`/`zoom-reset`），步进 0.5，`zoom` 事件把 `SetZoomLevel` 之后回读的 level 送回来。Chromium 自己按 origin 持久化，正好是内嵌组件想要的浏览器行为；宿主不需要在尺寸变化时替它重排 |
| 静音 / 音量 | `SetAudioMuted` / `IsAudioMuted` | **mute 已做**，音量不做 | `docs/audio-path.md` 把路径量掉了：页面出的声住在一个不沙箱的 `audio.mojom.AudioService` utility 里、直达宿主桌面，组件自己的开关只有这一个；它一次请求、无需回答，符合判据。音量是桌面混音器已经管着的事，不再画第二遍 |
| 下载 | `CefDownloadHandler::OnBeforeDownload` + 进度 + 取消 | **已做**（2026-10-21） | 当初判"不做"的理由是对的：它不是一问一答。解法不是硬塞进一问一答，而是给通道加一个**挂着不答的问答**——`OnBeforeDownload` 把 CEF 的门禁回调停在 helper 里，发一条 `download-begin` 就返回，答案由宿主在方便的时候用 `download-accept`/`download-reject` 送回来。宿主不答，文件就不落盘。细节见 `docs/download-path.md` |
| DevTools | `ShowDevTools` | **不做** | 那是第二个顶层窗口；嵌套合成器现在只合成一个 toplevel + 它的 popup（#21），要开就是 `xdg_toplevel` 的第二实例，属于协议侧的决定 |
| 历史列表 | `CefNavigationEntryVisitor` | **不做** | 现在只有 `can_go_back/forward` 两个布尔；列表是一次查询要一串回答，形状和页内查找同一类——那一条的做法（`id` + `final`）现在可以直接照抄，但还没有宿主提这个要求 |
| 清缓存 / 清 cookie | `ClearCache`、`CefCookieManager` | **不做** | 得先定义三档（缓存 / cookie / localStorage）分别意味着什么，这是隐私策略不是 API |
| 打印 / 存 PDF | `PrintToPdf` | **不做** | 形状和下载是同一套（挂住的回调 + `id` + 落盘路径），照抄就行；但 `docs/download-path.md` 那三条实测还没给它，不做没有证据的功能 |
| 自定义 UA | `CefRequestContext` | **不做** | 走环境变量比走命令合适：它是一次性设定，不该中途变 |
| 上下文菜单 | `CefContextMenuHandler::RunContextMenu` + `CefMenuModel` + `CefRunContextMenuCallback` | **已做**（2026-10-21） | 判据到这里已经不够用了：`download-*` 是"挂着等回答"，`find-*` 是"不发回答"，而 `context-menu` 是**必须回答**——CEF 只有这一条路把菜单交给宿主，`RunContextMenu` 返回 true 之后画与不画、答与不答全在宿主手上，而实测（`docs/context-menu.md` §2）证明 Chromium 同时只允许一个没答的请求，不答就等于把右键弄坏。形状是最省的一种：模型（`items` 五条属性）+ 坐标上线，答案下去，中间没有任何进度。`CefContextMenuParams` 留在 helper：模型本身已经是它的回答，`link URL` 这类"宿主想加自己的行"的信息是下一个增量，不是这次没有量的部分 |
| 权限提示 | `CefPermissionHandler` + `CefMediaAccessCallback` + `CefPermissionPromptCallback` | **已做**（2026-10-21） | 和菜单同一个判据：`OnRequestMediaAccessPermission` / `OnShowPermissionPrompt` 返回 true 之后，**页面就站在宿主的回答上**，不答不是安静而是 promise 永不 settle（基线实测：通知、剪贴板读、MIDI sysex 三条全 `pending`）。形状仍然最省：问题（origin + 名单 + mask）上线，一个词的答案回去，中间没有进度。两处"不能替宿主决定"留在原地：media 的 `accept` 只能整份给（`Continue()` 要求 `allowed` 等于 `required`），所以 API 上没开名单参数；反过来，`storage.persist()`、`permissions.query()` 和 idle 那三种问法根本不走 `CefPermissionHandler`（通知是走的，实测过），通道上因此没有它们——不是没做，是 CEF 那边没有可以问的地方（`docs/permission-prompt.md` §1、§10） |

反过来，**这 19 条够用**的证据还是这套组件的全部交互：导航（`load`/`back`/`forward`/
`reload`/`stop`）、页面缩放（`zoom-*`）、任意脚本（`js` 一条顶十条，页内状态、标题、当前 URL 都能自己读）、退出
（`quit`）、加上现在这几条要回话的（`download-*`、`find-*`、`context-menu-*`、`permission-*`）。要加的新命令
得先说明它不能用 `js` 表达——上面那张表里除了 DevTools，其余几条其实都能用 `js` 大致凑出来，
只是凑得不干净；下载**不能**凑，因为它落盘、要宿主给路径，而 `js` 那条通道一个字一个字都在
页面里。查找也**不该**凑：Blink 的命中序号、跨帧的"第 M 处"、以及把命中滚进视野那套行为都在
页面 JS 外面，用 `js` 自己实现等于在页面里再写一个查找，它会和页面自己的滚动、选区抢东西。
右键菜单也凑不了：`contextmenu` 事件里拿不到 CEF 那份模型（复制/剪切/粘贴那几条是编辑器状态
决定的，页面 JS 看不见 `IsEnabled`），更要紧的是它落不下去——Chromium 自己的菜单是一个 xdg_popup
（`docs/context-menu.md` §1），宿主想让它消失只有回调这一条路。

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

`find` / `find-stop` / `find-result` 这一组同样是先量的：`test/find-gate.sh` 六次运行
（四次假 helper、两次真 Chromium）、34 条断言，钉住 identifier 每请求递增、一条请求可以答
两到三条、无匹配只答一条、`match_case` 真的过线（8 对 7）、`find-stop` 静默，以及"搜索不动
页面、步进才滚进视野"。假 helper 那一半复刻的是**实测形状**而不是头文件措辞，所以两边能对上
号；四条语义和一次没归因的 flake 写在 `docs/find-path.md` §2、§4。

`meson test -C builddir` 14/14 照旧全过（那套测的是 compositor 侧，不碰控制通道）。

`context-menu` 这一组也是先量再做，量出来的三条都进了门禁：`test/menu-gate.sh` 八次运行
（六次假 helper、两次真 Chromium）、47 条断言。假 helper 那一半复刻的是 §2 那个"同时只有一个
没答的请求"，以及一份真机量到的模型（链接 2 行、图片 3 行、可编辑区 9 行、有选区 1 行，分隔符的 `id` 是
-1），所以"没接管就画默认菜单""接管之后答案只有一条""不答就再也问不出下一个"这些都能不开
浏览器钉住。真 Chromium 那一半钉的是通道的两端：`menu-pick 117` 之后页面里
`selectionEnd - selectionStart` 是 18，也就是 CEF 自己的全选命令真的在 Blink 里跑了；导航那次
钉的是会话替宿主兜住的那条路（`context menu dismissed by a navigation`），因为实测一次没答的
菜单**不会**被新导航 dismiss，popover 会跟着新页面一直挂着。菜单画在 `ChromiumView` 上——它
自己就是一个 xdg_popup 子面（`docs/context-menu.md` §1 量的正是这一点），所以 `shot` 抓不到它，
真机那半的"画出来了"证据用的是会话自己那条 `context menu mapped=1 size=` 调试行。

`permission-*` 这一组同样是先量再做，而且量的次数比前几组多——因为它的规则不在头文件里。
`test/permission-gate.sh` 十次运行（七次假 helper、三次真 Chromium）、58 条断言。假 helper 复刻的是
`docs/permission-prompt.md` §2 那两条队列和顶掉：media 槽满时后来的 media 问法不上线、答掉之后才作为
新 id 到达；prompt 槽满时先补一条 `permission-dismissed ignore` 再上新的一条。真 Chromium 那一半钉的是
**页面上发生了什么**：`real-deny` 逐字对十项回读（同一篇 §1 的右列），`real-answer` 证明宿主给的答案
真的走到了页面（定位 deny → `code1`，通知 accept → `granted`），`real-park` 用四次回读加两条**顺序**断言钉住 media 队列是浏览器的
而不是仿造品的——为了这件事门禁里新加了一个 `expect_after`（比较两处匹配的行号），两次 grep 说不清
"这条请求是那条答案放出来的"。三次真机运行各自一份 `XDG_CACHE_HOME`，于是 helper 每次冷启动要三秒
才报 `ready`：正是这个把 demo 里一个早就存在的坑照了出来——会话脚本原来靠 800 ms 定时器起步，第一条
`load` 发在通道打开之前，整段脚本静默地什么都没做。现在 session 模式下脚本从 `::ready` 起步。

## 6. 还没做的

* **一条命令都没有应答机制**。`js` 有 `id`，其余命令发出去就完了；宿主不知道 `load` 有没有
  被吃到（`address`/`load-end` 是间接证据）。`find` 是**故意**不发 ack 的——它的回答全是事件，
  再配一条 `ok` 只会让宿主以为是两个答案。参数不合法（空 `pattern`、空 `path`）现在统一回一条
  `status`，那是日志不是回执。要不要给每条命令配一个 `ok`，等 §4 里出现需要它的那条再说。
  现在得补一句：这个判据在 `context-menu` 上**不成立**了。它反过来是一条必须回答的问题，"没有
  ack"不再是安全默认，所以会话侧给它加了一条不变量：**`context-menu` 请求一定活到一个答案**
  （要么宿主的 `select`/`dismiss`，要么会话自己 dismiss）。`no-such-download` 那种"日志而不是
  回执"在这里也用了——宿主答一个已经结束的菜单，helper 只回 `status`。**`permission-request` 是同族里
  更贵的一个**：菜单不答是下一次右键安静地没了，权限不答是页面的 promise 永远不 settle（基线三条
  `pending`），实测还额外赔上"紧接着的下一次问法凭空消失"。所以会话给的默认只有一个方向：deny。
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
* **查找只能一步步走**。CEF 的 `Find` 没有"跳到第 N 处"，`docs/find-path.md` §2.1 还量到
  步进报的序号偶尔会偏（三次里有一次第一步报 3 而不是 1）。所以宿主想要"第 5 处"，就得发
  五次步进，而且不能假定落在哪一条上——这是 Blink 的内部顺序，不是通道契约。
* **有一次真机 find 门禁一条事件都没收到**，逐字复现和之后的运行全都正常（`docs/find-path.md`
  §4）。没有第二次复现，不下结论；如果它再现，先查 helper 的收尾时序，别先改协议。
* **上下文菜单上线的是模型，不是问题**。`CefContextMenuParams` 一个字都没过线，所以宿主现在只能
  *重画* CEF 给的菜单，不能*扩充*它：「复制链接地址」「图片另存为」需要 `link_url` / `src_url`，
  而 §4 那张表里量到的默认模型对链接、图片、空白页是**同一份 5 行**（libcef 就给这些，多的行是
  cefsimple 自己加的）。把 params 上线是这一条剩下的全部实质工作，形状不难（一次查询、无回答），
  但得先有宿主真的需要加行——`docs/context-menu.md` §3 把这条判据写在了那里。
* **一条挂起的菜单请求在 helper 退出时会怎么样，没测**。`context-menu-select` 发不出去、宿主那边
  `menu_pending` 还立着、session 已经关了。这是 §6 第一条（没有 ack）真正疼的地方：宿主没法知道自己
  的回答有没有吃到。等 `docs/session-lifecycle.md` 那侧有结论一起看。
* **一条挂起的 media 请求同样没测，而且它连回调都没有**。`CefMediaAccessCallback` 上只有
  `Continue`/`Cancel`，CEF 不会关一条宿主扣着的 media 请求（prompt 会，`permission-dismissed`），所以
  换页、关窗、helper 退出这三种情况下那条 `getUserMedia` 变成什么，通道上没有任何证据。
  `docs/permission-prompt.md` §10 把这条列在"没做"里而不是"没写"里。
* **prompt 之间那条"吞掉"规则没有解释**。面板扣着一条 prompt 时，页面紧接着的下一次问法既不到宿主、
  也不再补来（复现两次，`docs/permission-prompt.md` §3）。它没有对应的宿主侧动作——唯一的对策是每条都
  答，而那已经是门禁钉的不变量——所以 fake 没有复刻它，别把它当成仿造品漏掉的特性。
* **`DISMISS` 这个答案只在假 helper 上走过线**。真机上它和 `DENY` 在页面上的区别没测（media 那边
  根本没有第三种答案：`Cancel()` 把"收起"和"拒绝"合成一件事）。
* **`storage.persist()`、`permissions.query()`、idle detection 到不了宿主**。不是没做：它们不走
  `CefPermissionHandler`，helper 这边没有一个可以问的地方。想在宿主侧管持久化存储的许可，得等 CEF
  接线，或者绕到 content settings 那一层——那是另一个 API。
* **`--enable-media-stream` 是一个回归地雷**。头文件明说：带这个开关时
  `OnRequestMediaAccessPermission` **不会被调用**，它等于对摄像头和麦克风全部放行。现在 helper 没加它，
  将来谁加开关时应当先看 `real-park` 有没有变红。

## 通道读这一侧：缺字段和没有换行的流

日期：2026-10-22。来源：`code-review-2026-10-22.md` **M14**、**M10**。

### 1. 缺一个字段不是"这条事件坏了"，是"这个字段没有值"

helper 是另一个二进制，而且是**另一个版本**——这在一条 unix socket 上是常态，不是事故。§3 那句
"unknown event 整条打出来"早就承认了事件的名字可以陌生；可字段一旦被直接
`json_object_get_*_member` 取，就等于假设了字段一定在。json-glib 对不在的 member 的回答是
**一条 critical 加一个值**：字符串给 NULL，数字和布尔给 0。NULL 会一路走进宿主自己声明为
`const char *` 的信号参数；0 更麻烦，因为它是一个**会被下游相信的值**——`id=0` 的下载、0 % 的进度、
`(0,0)` 的菜单，每一个都有代码在替它做决定。

把 `src/chromium-session.c` 里那五个包装退回直接 getter（就是改造前的形状），一条只说事件名的流
（28 行）之后宿主是这样的：

```
FAIL no-fields: the demo died on signal 11 (124 = it had to be killed)
FAIL no-fields: 18 critical(s) — a missing member was met as if it were present
             8 json_object_get_string_member: assertion 'node != NULL' failed
             7 json_object_get_int_member: assertion 'node != NULL' failed
             2 json_object_get_double_member: assertion 'node != NULL' failed
             1 json_object_get_boolean_member: assertion 'node != NULL' failed
FAIL no-fields: a NULL reached a signal the host documents as a string
      39:demo: download 0 begin name=download mime=(null) bytes=0 url=(null)
      49:demo: download 0 end state=(null) path=(null) bytes=0
```

所以这一条不是代码卫生，是**对端少发一个字段就能把宿主打死**：18 条 critical 之后是 SIGSEGV。
`test/download-gate.sh` 那套断言里 `mime=` 是拿来判断"是不是我们认识的那个类型"的，`(null)` 进去
它只会当成未知——错的形状不会报错，只会安静地变成另一个结论。

现在的答法是五个包装，缺字段各自折成该类型的空值：`event_string` → `""`、`event_int` → `0`、
`event_flag` → `FALSE`、`event_number` → `0.0`、`event_bytes` → `-1`。最后那个不是随手选的：
`-1` 是 CEF 自己"长度从来没人告诉过我"的写法，`docs/download-path.md` 里 `percent=-1` 用的同一个
约定，宿主已经在处理它了。

判据写在这里，免得下次再争：**缺字段折成空值，不发警告、不拒收**——helper 有权少发一个字段，那是
它的自由，宿主没有理由为它记一笔；真正算错误的是**整条不像 JSON**，那走 §3 那条 unknown event 的
日志，形状早就定了。

### 2. 门禁一：`test/event-shape-gate.sh`

它不开浏览器：`CHROMIUM_HELPER` 指着一段现写的 python，连上通道，每条事件只发 `{"event": 名字}`。

* **名单从源码里读**，不是手抄：`grep -oE 'strcmp \(event, "[a-z0-9-]+"\)' src/chromium-session.c`。
  库里加一条事件，门禁就多发一条；漏发就是这道门失效的方式，所以它反过来也有下限——不足 20 条
  直接红，并且说清"this gate would be grading nothing"。
* **它覆盖的是宿主读的那一侧，不是 helper 发的那一侧**，这个差别值得记下来：helper 的
  `NewEvent` 一共发 24 个**字面量**名字，全部命中；名单里另外 4 个（`copy-text`、`open-url`、
  `page-message`、`primary-selection`）也是同一个 `NewEvent` 发的，只是名字写在变量或常量里
  （`helper/src/main.cpp:907`、`1804`、`1813`，常量在 `81`、`86`），所以按字面量 grep 抓不到。
  也就是说"事件表里加了名字但忘了接线"这种事，这道门会替你发出去。
* 三条断言：宿主自己回来（不是被 `kill`）、**一条 critical 也不许有**、`(null)` 一个也不许出现。
  最后一条抓的是 NULL 已经流进信号之后**宿主自己的打印**，所以它不需要宿主改代码就能测到。

跑（它开真窗口，按 `test/framing-gate.sh` 同一个约定算手跑门禁）：

```
./test/ci-session.sh ./test/event-shape-gate.sh
  ok   event-shape: 28 event name(s) read out of the dispatch
== no-fields
  ok   no-fields: the demo came back with status 1 after being told the browser closed
  ok   no-fields: no critical from 28 event(s) with no fields
  ok   no-fields: nothing was (null) on the way to the demo
0 failure(s), 0 skip(s)
```

第一条 `ok` 报的是名单里有多少条——它是这道门自己的体检。最后一条是**只有名字**的流走完之后宿主
还活着；`status 1` 是那条 `closed` 该产生的结果（会话被关掉，脚本还剩一步），不是错。

变异就是 §1 抄的那段转录：18 条 critical、两条 `(null)`、外加 SIGSEGV，三条断言一起红。同一天
15:21 又复跑了一次（把五个包装的 `json_object_has_member` 判断替换成 `TRUE`），逐字相同：
`died on signal 11`、`18 critical(s)`、8/7/2/1 的分布。这里要订正门禁注释里先前写下的一句
"十五事件 22 critical"——那是更早的一次数出来的，现在的 28 条只数到 18 条 critical，但**多了那次
没有的段错误**；`test/event-shape-gate.sh` 的注释和 `src/chromium-session.c:283-288` 的那句现在都
按最新一次改过了。

### 3. 一条没有换行的流：读侧的 framing

`g_data_input_stream_read_line_async` 会把 buffer 长到对端给出换行为止。这句话翻译过来是：**对端
的想象力有多大，这里就分配多大**。S4 那次把写侧的部分写补成了循环，读侧一直还是原样。

现在宿主自己按 8 KiB 一块读（`CONTROL_CHUNK`），自己切行，未凑齐一整行的字节留在
`self->line_pending` 里，越线就拒收：

* 上限 `CONTROL_LINE_MAX = 4 MiB`（`4u << 20` = 4194304，`src/chromium-session.c:30`）。挑这个数
  不是为了"给合法行留余量"——helper 那边**有上限的那几条**里最大的是 `js-result`，序列化出来超过
  65536 字节就整条丢掉（`helper/src/main.cpp:91` 的 `kMaxResultBytes`、`1221-1226`），可 `title`
  和 `address` 那边一个字节上限都没有（`1979-1994` 直接 `SetString`），页面写多少字节的标题就有
  多少字节过线。所以这个数的含义是"到这里为止我不再相信这条流"，不是"到这里为止还能忍"。
* 撞线 = 一条 warning + 拆连接 + `mark_closed ("control line too long")`。比较是**严格大于**
  （`line_pending->len > CONTROL_LINE_MAX`，`src/chromium-session.c:728`）：恰好 4 194 304 字节还不
  撞线，多一个字节才撞。为什么不学 helper 那样丢掉
  buffer 继续服务：宿主这边 `mark_closed` 是公开 API 里**唯一**能表达"通道不可信"的词，而拆掉之后
  状态机会自己把页面报成没了；留着连接等于让 `ready` 继续立着、后面每一个事件都没人解析。
* **NUL 单独处理**。`{"event": "status", "message": "before"}\0after\n` 是这条最阴的形状：前半段是
  一个**完整**的 JSON 对象，任何一个按字符串读的解析器都会把它当成一条事件、把 `after` 扔掉——于是
  宿主报告了一件 helper 从没发过的事，而且哪里都不报错。所以 `length != strlen (line)` 就把**整行**
  丢掉（不截断），warning 里点名 NUL 在第几字节（这一条形状下是 `at 40 of 46`：`before` 那个对象
  40 字节、NUL 在第 41 个字节、`after` 再 5 字节，都不算换行），之后的流照常。
* `feed_control_bytes()` 每吐出一行都要回头看一眼 `line_pending` 还在不在：`handle_event()` 里面
  一条 `shutdown`、任何一次发送失败都可能把通道拆掉，而那个 buffer 是被拆掉的那一半 free 的。

**helper 那一侧早就有同一道规矩**（1 MiB 就丢弃 buffer），所以这条通道两端的上限并不对称——1 MiB
配丢弃、4 MiB 配拆连接，两边各自说得通，但只有宿主这一半有门禁，helper 那一半没有。列在 §5。

### 4. 门禁二：`test/framing-gate.sh` 的 `flood` 段

一次运行同时量三件事：一条**合法的大行必须整条读到**、一条带 NUL 的行必须**丢掉而不是半信**、
一条没有换行的流必须**撞上限**。第一条是这个改动最大的风险所在——真把那条一百万字的合法行
（整个 1 000 034 字节，`test/framing-gate.sh:207`）误杀，比放过一个洪水攻击严重得多，所以它和
另外两条一样是一条断言，不是一句承诺。

绿（`./test/ci-session.sh ./test/framing-gate.sh`）：

```
  ok   flood: the demo came back on its own (status 1)
  ok   flood: /over the [0-9]+ byte ceiling/
  ok   flood: /helper stopped \(control line too long\)/
  ok   flood: the 1000000-byte legal line was read whole
  ok   flood: /control line holds a NUL at [0-9]+ of [0-9]+/
  ok   flood: nothing was handled out of the NUL line
```

**变异 B**（删掉 `line_arrived()` 里那句 NUL 判断，回到"按字符串读"的形状）→ rc=1，两条红：

```
  FAIL flood: wanted /control line holds a NUL at [0-9]+ of [0-9]+/
  FAIL flood: the bytes before a NUL were read as an event
         9:** Message: 10:54:39.134: session: helper status: before
```

`helper status: before` 就是那个"看起来完全合理、helper 却从没发过"的事件：warning 没了，宿主照样
把它当成一条 `status` 报了。NUL 之后的 `after` 则并进后面那段洪水里，一起被上限拦掉。

**变异 A**（删掉 `feed_control_bytes()` 里的上限判断，就是 `read_line_async` 的原样）→ rc=1，那两条
关于上限的断言红：

```
  ok   flood: the demo came back on its own (status 0)
  FAIL flood: wanted /over the [0-9]+ byte ceiling/
  FAIL flood: wanted /helper stopped \(control line too long\)/
  ok   flood: the 1000000-byte legal line was read whole
  ok   flood: /control line holds a NUL at [0-9]+ of [0-9]+/
  ok   flood: nothing was handled out of the NUL line
```

红的方式很安静，而这正是这条要抓的：5 242 880 个没有换行的字节照单收下，宿主一声不响，`status` 从 1 变回
0——会话不是被上限关掉的，是脚本自己跑到 `close` 关掉的。**这段运行里宿主唯一的"症状"是它把那 5 MiB
全存进了 `line_pending`——这一步是代码形状给的，RSS 没有量过；有没有真的多占几兆、占多少，没人测**。
同一份运行里合法大行、NUL 两条仍然是绿的，所以这两条断言不是顺带过的，它们各自只对应一个能删掉的东西。

### 5. 各拦什么，以及还没做的

| | `event-shape` | `flood` |
| --- | --- | --- |
| 缺字段 → critical / NULL / 0 | **红**（18 条 critical + 段错误） | 不管这个方向 |
| 无换行的流 → 内存无界 | 看不见（每条都短） | **红**（变异 A：宿主不响，`status` 变回 0） |
| 行内 NUL → 半条被当成一条 | 看不见（不发 NUL） | **红**（`helper status: before`） |
| 合法大行被误杀 | 不管 | 那条断言就是反过来用的：变异里它**必须**保持绿 |

* **helper 那一半的 1 MiB 上限没有门禁**。它连丢弃之后是否还能接住下一条完整命令都没人验过。两端
  不对称本身是决定，不是 bug，但"只有半边有门"这件事得记着。
* **上限之后没有"重新同步"**。撞线就拆连接，是干脆的；但 helper 那边丢弃 buffer 之后继续收，如果它
  丢的正好是半条命令，剩下那半会被当成新命令的开头。真实形状是没测过的。
* **`js` 结果那 64 KiB 是**丢掉整条 value**，不是截断**（`truncated=true` + `value=null`）。
  宿主现在不会收到半条结果，但也不会知道少了什么——`message` 里那句"result exceeded 64 KiB and was
  dropped"是唯一线索。
* **缺字段一律折成空值之后，宿主没法区分"页面标题真的是空"和"helper 没发 title"**。这是这条判据
  的价格，现在没人抱怨；真要区分得给通道加"字段在不在"的表达，那是协议改动，不是这条的门禁能定的。

## 页面 ↔ 宿主消息通道

控制通道原本只有"宿主 ↔ helper"两条腿；页面要和宿主说话得穿过两层。现在
两个方向都有了约定：

**页面 → 宿主（推送）**：renderer 在每个 context 注册
`cefnestPostMessage(jsonString)`（READONLY|DONTENUM|DONTDELETE，页面遍历
自己的 globals 不会踩到）。参数是**一个 JSON 字符串**——payload 内的 schema
由宿主自定，通道只保证字符串完整送达。**这条路两端都没有大小上限**：js 结果
回传的 64 KiB 上限（`kMaxResultBytes`，`helper/src/main.cpp:91`）只用在
`EmitJsResult`（`:1222`）那一条路上，`PostMessageHandler`（`:1642`）→
browser 进程转发（`:1802-1808`）→ `page-message` 事件原样透传，没有 cap、
没有 `page-message-dropped` 事件（早先的文档写过这个事件，代码里从未实现，
2026-10-22 全源码检索确认）。后果是一条超长的页面消息就是一条超长的通道事件，
归宿主侧 4 MiB 行上限管——越线是整条会话拆掉，不是消息截断。一个很大
payload 在 CEF 进程间消息内部的形状没测过，照原样送达属于未验证。

**宿主 → 页面**：用现成的 `run_js`——约定俗成的接收方式是页面自己挂
`window.addEventListener('cefnest-message', e => …)`，宿主侧
`run_js("window.dispatchEvent(new MessageEvent('cefnest-message',{data:" +
JSON.stringify(msg) + "}))")`。

**协议**：

```
renderer → browser → host
  {"event":"page-message","json":"{\"type\":\"ping\"}"}

host → helper → 页面
  {"cmd":"send-message","json":"…"}   （MVP 未做，run_js 覆盖）
```

**验证**：`/tmp/msg.html` 按钮调 `cefnestPostMessage`，
`demo: page-message {"type":"ping","n":1,"text":"hello from the page"}`
出现在宿主日志（2026-10-21，真 CEF）。

## 弹窗策略：target=_blank 与 window.open

Alloy 下页面开新窗口（target=_blank、window.open、中键）会触发
`OnBeforePopup`。单视图组件无法显示第二个 toplevel，策略是**原地导航**：

- 前台类请求（FOREGROUND_TAB/NEW_POPUP/NEW_WINDOW/SINGLETON_TAB）→ 当前视图
  直接 `LoadURL(target_url)`（kiosk 式就地打开，历史条目正常生成）；
- **后台标签（NEW_BACKGROUND_TAB）只上报不导航**——它明确表示不想抢视图，
  宿主可通过 `popup-requested` 事件自行收集；
- 每个请求都发 `{"event":"popup-requested","url":…,"disposition":N,"gesture":…}`
  事件，宿主可观察或将来接管。

**验证**：`/tmp/popup.html`（target=_blank 链接）点击后 demo 原地导航到
`https://example.com/popup-target`，`popup-requested` 事件与 `back=1` 历史
条目齐备（2026-10-21，真 CEF）。
