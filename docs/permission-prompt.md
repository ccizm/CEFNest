# 权限提示：谁能问、谁必须答，和一条扣住的请求会吞掉哪一条

日期：2026-10-21
问题：P2 的「权限提示 UI（1~2 天）」，任务 #57–#62。立项那句话写的是「页面要麦克风时宿主得问人」，
实测之后**这句得改**：Alloy 那边的 Chromium 早就替宿主拒绝了，页面并不是停在原地等宿主——除了三种
连问都不问的。这一篇钉的是只有实测才给得出的四件事：哪几种问法根本到不了宿主、两条互不相干的队列、
一条"扣着不答就连下一条也看不见"的规则，以及 `accept` 为什么只能整份给。

## 1. 动手之前：没有 handler 时长什么样

`test/permission-page.html` 十个按钮各点一次，页面自己回读（任务 #57 的基线，那时 helper 里还没有
permission handler）：

| 页面问的东西 | 没有 handler | 有 handler、一律 deny |
| --- | --- | --- |
| `getUserMedia` 音频 / 视频 / 两者 | NotAllowedError | NotAllowedError（**一模一样**） |
| `geolocation.getCurrentPosition` | `code3` POSITION_UNAVAILABLE | **`code1` PERMISSION_DENIED** |
| `Notification.requestPermission()` | **永远 pending** | `denied` |
| `clipboard.readText()` | 永远 pending | NotAllowedError |
| `requestMIDIAccess({sysex:true})` | 永远 pending | NotAllowedError |
| `navigator.storage.persist()` | `false` | `false`（根本不问） |
| `permissions.query({name:'camera'})` | `prompt` | `prompt`（根本不问） |
| `navigator.idle` | 没有这个 API | 同上 |

三条 `pending` 才是这一条存在的理由。头文件说得直白：Alloy 下 media 的默认处理是 deny，prompt 的默认
处理是 `CEF_PERMISSION_RESULT_IGNORE`——而 IGNORE 落到页面上就是**一个没人回答的 promise**。geolocation
那一行的区别（`code3` → `code1`）是同一件事的另一面：宿主不答，页面连"我是被拒了，还是这台机器根本没有
定位源"都分不出来。

顺带纠正一个我一开始记错的：**`Notification.requestPermission()` 是会过线的**
（`helper: permission 1 prompt from file:/// wants notifications`，raw=32768）。基线里那个 `notify:pending`
是"没有 handler"，不是"问不到宿主"。根本到不了的只有 `storage.persist()`、`permissions.query()` 和
idle detection——它们在 Blink 里就不走 `PermissionRequestManager`。

## 2. 两条队列，互不相干，一条各只有一个槽

**media 是串行的。** 一次 `getUserMedia` 没答，后面每一次 media 问法**一个字都不会到宿主**；答掉之后，
被扣住的那一次才作为**它自己的新 id** 到达。`real-park` 钉的就是这个，页面的四次回读是证据本体：

```
helper: permission 1 media from file:/// wants device-audio     ← 点麦克风
                                                            ← 点摄像头：通道上什么都没有
  页面 1：audio:pending|video:pending
helper: permission 1 answered deny                          ← perm-deny 1
  页面 2：audio:NotAllowedError|video:pending                ← 摄像头还在等
helper: permission 2 media from file:/// wants device-video  ← 它现在才是请求 2
  ...（点麦克风+摄像头 → 请求 3 = device-audio+device-video）
```

**prompt 会互相顶掉。** 扣着的 prompt 不会等下一个，Chromium 在把下一个交给宿主**之前**先把旧的关掉。
单独一份探针，冷 profile、`ask` 策略、全程一个都不答，依次点定位 → 剪贴板 → 通知 → MIDI：

```
helper: permission 1 prompt ... wants geolocation      ← 点定位，面板扣着
helper: permission 1 dismissed as ignore               ← 点剪贴板
helper: permission 2 prompt ... wants clipboard
                                                    ← 点通知：一个字都没有（§3）
helper: permission 2 dismissed as ignore               ← 点 MIDI
helper: permission 3 prompt ... wants midi-sysex
```

`OnDismissPermissionPrompt` 的措辞是「`result` 为 `IGNORE` 表示因为别的原因关掉了」，这里就是别的原因。
**media 没有这一条**：`CefMediaAccessCallback` 上只有 `Continue`/`Cancel`，CEF 不会替宿主关一条 media 请求，
所以宿主的行只能自己知道"这条还挂着"。

**两条队列互不影响。** 一次探针把麦克风扣住，然后依次点定位、摄像头、剪贴板：

```
helper: permission 1 media  ... wants device-audio
helper: permission 2 prompt ... wants geolocation      ← media 扣着，prompt 照来
helper: permission 2 dismissed as ignore
helper: permission 3 prompt ... wants clipboard
页面：audio:pending|geo:pending|video:pending|clipboard:pending
                                          ↑ 摄像头那次：media 槽被 1 占着，根本没到宿主
```

合起来，宿主同一时刻最多握着**一条 media + 一条 prompt**。这和菜单那种"全局只有一个槽"不一样，所以
`src/chromium-session.c` 里握的是一个 `GHashTable`，而不是一个布尔：id 是两张队列共用的一个递增序号，
槽位则是各自的。

## 3. 最贵的一条：扣着的 prompt 会吞掉紧接着的下一次问法

两次独立探针都是这个形状——**面板还挂着一条 prompt 时，页面上紧跟着的下一次问法既不到宿主、也不被
顶掉，它就是消失了**：

```
helper: permission 1 prompt ... wants notifications   ← 点通知，扣着
                                                 ← 点定位：通道上什么都没有，页面上永远 pending
helper: permission 1 dismissed as ignore        ← 点剪贴板：这条才把 1 顶掉
helper: permission 2 prompt ... wants clipboard
```

（另一头也复现过：剪贴板那条被扣着时，紧接着的通知同样一个字都没到宿主——就是 §2 那份四连点转录里
的第三次点击。）

这条不是"队列在排队"：排队的 media 请求**答掉之后一定会来**（§2），被吞掉的 prompt 再也没有出现过。
对照组是 `real-deny`：十次点击、每一条都被 session 立刻 deny 掉，七条请求**一条不丢**，丢的三条
（`persist`、`query-camera`、`idle`）本来就不经过这套机制。所以能下的结论是操作性的：
**答得越快，页面能问的东西越多**。这条和上面那条"必须回答"的规则是同一件事的两半——菜单不答是
右键坏掉，权限不答是页面挂住、并且后面的问法会凭空消失。

（fake 复刻了 §2 的两条队列和顶掉，**没有**复刻这条吞掉：那是 Chromium 内部 `PermissionRequestManager`
的状态，宿主这一侧唯一能做的就是把每一条都答了，而那已经是门禁在钉的东西。写在这里是为了别让后人去
给 fake 补一条它测不到的规则。）

## 4. 形状：两条事件、一条命令

| CEF 侧 | 通道上 |
| --- | --- |
| `OnRequestMediaAccessPermission(browser, frame, origin, mask, CefMediaAccessCallback)` → return true | `permission-request`：`id`、`kind="media"`、`origin`、`permissions[]`、`raw` |
| `OnShowPermissionPrompt(browser, prompt_id, origin, mask, CefPermissionPromptCallback)` → return true | 同上，`kind="prompt"` |
| `permission-respond` + `accept` | media：`Continue(required)`；prompt：`Continue(ACCEPT)` |
| `permission-respond` + `deny` | media：`Cancel()`；prompt：`Continue(DENY)` |
| `permission-respond` + `dismiss` | media：`Cancel()`（没有第三种答案）；prompt：`Continue(DISMISS)` |
| `OnDismissPermissionPrompt(browser, prompt_id, result)` | `permission-dismissed`：`id`、`result`，**只在 helper 还握着那条时发** |
| `CefClient::GetPermissionHandler()` | `helper/src/main.cpp` 的 `HelperPermissionHandler` |

`id` 由 helper 自己发（`++g_permission_seq`，media 和 prompt 共用），不是 CEF 的 `prompt_id`；CEF 那个
只留着用来认 dismiss 回调。`permission-respond` 带一个不认识的 id 回一条 `status: no-permission-request`
——日志，不是回执，和下载/菜单同一套。

session 侧的 `attach_connection` 会清空握着的那张表：helper 重启后序号重新从 1 开始，旧 id 会答到一条
不属于它的请求上。

## 5. `raw` 和名字都上线，因为要匹配的宿主不想从名字反推

| 问法 | kind | raw | names |
| --- | --- | --- | --- |
| 只要麦克风 | media | 1 | device-audio |
| 只要摄像头 | media | 2 | device-video |
| 两个都要 | media | 3 | device-audio+device-video |
| 剪贴板读 | prompt | 16 | clipboard |
| 定位 | prompt | 256 | geolocation |
| MIDI sysex | prompt | 8192 | midi-sysex |
| 通知 | prompt | 32768 | notifications |
| `storage.persist()` 那条 | prompt | 1048576 | storage-access |
| 本地网络 + 回环 | prompt | 201326592 | local-network+loopback-network |

前七行**都是通道上量到的**（`demo: permission … raw=` 那一行就是证据，门禁断言的就是它）。最后两行
只有头文件的值：`storage.persist()` 根本不过来（§1），本地网络这条没能在页面上造出来。它们进 fake 的
`PERM_ASKS` 不是冒充实测，是要让宿主侧的解析代码在无浏览器的时候也能跑到 2^26/2^27 这种量级——
`CefDictionaryValue` 只有 `SetInt`，大数走的是 JSON 的 double，这条通道上唯一会碰到的就是这类 mask。

这些位**公共头文件一个都不列**，只在 `include/internal/cef_types.h` 里（`cef_media_access_permission_types_t`
和 `cef_permission_request_types_t`）。名字给人看（面板上一行
`prompt · geolocation · file:///`），位给想精确匹配"就是这几个 bit"的宿主；`local-network+loopback-network`
那种一条请求带两个 bit 的，只有名字会迫使宿主自己做集合运算。

## 6. `accept` 只能整份给，所以 API 上没有 `accept(names)`

`CefMediaAccessCallback::Continue(allowed_permissions)` 的头文件原话是：getUserMedia 的请求
（DEVICE_AUDIO / DEVICE_VIDEO 位）**`allowed_permissions` 必须和 `OnRequestMediaAccessPermission` 收到的
`requested_permissions` 一致**。所以 helper 把 `required` 存进槽里，`accept` 就是 `Continue(entry.required)`。
这决定了 API 的形状：**宿主没有"只给麦克风、不给摄像头"这个权力**——想只给一半只能拒绝。既然
`accept` 永远是"整份给"，`chromium_session_permission_respond (id, result)` 就只要一个 result，不多开一个
没人能兑现的名单参数。

prompt 那边没有这个约束（`Continue(result)` 就三个值），但 `DISMISS` 和 `DENY` 在页面上的区别**没有实测**：
demo 的 拒绝 只发 DENY，`perm-dismiss` 步骤发 DISMISS，门禁里 DISMISS 只在 fake 上走线（`fake-yank`）。

## 7. 答过的权限下次照问，profile 只是运行之间的隔离

在同一个 profile 里连着点两次定位：

```
helper: permission 1 prompt ... wants geolocation
helper: permission 1 answered deny
helper: permission 2 prompt ... wants geolocation    ← 同一份 profile，第二次还是问宿主
helper: permission 2 answered deny
页面：geo:code1|geo:code1
```

也就是说**没有"答过一次就静默"**（至少 `file://` 这条路上没有），宿主的策略每问一次都会真的执行一次。
`test/permission-gate.sh` 给每次真机运行单独一个 `XDG_CACHE_HOME`，为的是**运行之间**互不遗留状态——
不是防一条被记住的答案（这个理由我先写进过门禁注释，量完上面这两条之后改掉了）。

## 8. 宿主侧：四个策略、一层行、两道重复回答的闸

`CEFNEST_PERMISSION`：`ask`（缺省）留一行等 允许/拒绝 或 `perm-accept`/`perm-deny`/`perm-dismiss` 步骤；
`deny` 与 `allow` 一到就答、行只留着当记录；`ignore` **不接管信号**，于是 session 替它拒绝——这正是
"没有权限 UI 的宿主"，也是库的默认。

三条和菜单同形的规则：

- `::permission-request` 返回 TRUE 表示"我画，我负责回答"；返回 FALSE 或不接，session 立刻 deny 并记
  `session: permission 1 (media) from ... host took it: 0`。
- 重复回答有**两道**闸：面板那层 `demo: permission %d was already answered`（`decided` 的行、以及被
  Chromium 顶掉的行都算死），session 那层 `session: no permission request %d to answer`。面板先挡，所以
  带面板的跑里看不到后一条——这不是漏测，`fake-ignore` 专门用不接管的方式跑了一遍，那边看到的是
  session 自己发出去的那条 deny。
- `::permission-dismissed` 到了，行里的 允许/拒绝 按钮**必须消失**（"页面已经不再等这个答案"）：那条请求
  在 helper 那边已经不在了，按下去只会得到一句 no-permission-request。

## 9. 门禁

`./test/permission-gate.sh`（`fake` / `real` / 两者）十次运行、58 条断言。**全程没有打开过麦克风或摄像头**：
每一条 media 请求最后都是 deny，唯一断言过的 accept 是通知，它背后没有设备。

| 场景 | 钉住的东西 |
| --- | --- |
| `fake-accept` | 一行逐字段一致；accept 过线；答第二次的权限被面板挡下；新 id 才活 |
| `fake-yank` | prompt 顶掉 → 旧行变死；对死 id 的 `perm-accept` 不过线；`dismiss` 过线 |
| `fake-media` | media 串行：恰两条请求，第二条**在**第一条的答案**之后**（`expect_after` 比行号） |
| `fake-deny` / `fake-allow` | 一到就答、行只当记录；一条请求带两个 bit（201326592）时 names 是两条 |
| `fake-ignore` | 不接管 → `host took it: 0` + session 自己 deny，面板一个字不说 |
| `fake-auto` | helper 自己关掉请求 → 行死、不发第二条；`perm=vibration` 回 `unknown-permission` |
| `real-deny` | 十次点击七条请求，页面回读十项**逐字**一致（§1 右列） |
| `real-answer` | deny 定位 → 页面 `code1`；accept 通知 → 页面 `granted`（答案真的走到页面） |
| `real-park` | §2 的 media 队列是 Chromium 的：三条请求、两次顺序断言、页面四次回读 |

`expect_after` 是这次新加的比较器：队列讲的是**顺序**，两次 grep 说不清"这条请求是那条答案放出来的"。
真机三次运行各自一个 `XDG_CACHE_HOME`（§7）。它们也让每次运行冷启动一份 profile，于是 helper 要
三秒左右才报 `ready`——正是这个把 demo 里一个一直存在的坑照出来了：**会话脚本原来是靠 800 ms 定时器
起步的**，冷 profile 下第一条 `load` 发在通道打开之前，整段脚本静默地什么都没做。现在 session 模式下
脚本从 `::ready` 起步（`session_ready`），没有 helper 的纯 Wayland 客户端那条路才保留原来的定时器。

`meson test -C builddir` 10/10、`test/audit-impl-slots.py` 0 uncovered，find / menu / download / dnd 四套门禁
在改完之后各自重跑全绿。

## 10. 还没做的

- **三种问法宿主永远看不到**（§1）：`storage.persist()`、`permissions.query()`、idle。想在宿主侧决定
  持久化存储的许可，得等 CEF 把它们接到 `CefPermissionHandler` 上，或者绕到 content settings 那一层。
- **§3 那条吞掉规则没有解释，也没有办法测在宿主身上**。它能推出的唯一要求是"每条都答"，而 fake 没有
  复刻它（§3 末）。
- **`DISMISS` 在页面上的效果没测**（§6 末）。fake 走线，真机没走。
- **导航会不会关掉一条 prompt** 只有头文件的说法（"browser closure, etc."），没测。media 那条更干脆：
  CEF 根本没有 media 的 dismiss 回调，页面换页时扣着的 `getUserMedia` 会变成什么，没量过。
- **没有"记住这个选择"的宿主层策略**。每次问都问宿主（§7），demo 的面板也不写任何持久状态；真要做
  per-origin 策略，得先决定它存在哪（宿主的表？Chromium 的 content settings？），那是产品决定不是 API。
- **`--enable-media-stream` 是一个回归地雷**：头文件明说带这个开关时 `OnRequestMediaAccessPermission`
  **不会被调用**（它等于"全部放行"）。现在 helper 的 `OnBeforeCommandLineProcessing` 没有加它，将来谁加
  开关时应当先看 `real-park` 有没有变红。
