# 插件

[English](plugins.md) | 简体中文

插件是什么、能做什么、永远不能做什么。方法与信号速览见
[api.zh-CN.md](api.zh-CN.md);响应过滤(拦截二期)规划在
[`docs/plans/response-filter-plan-2026-10-26.md`](plans/response-filter-plan-2026-10-26.md)。

## 形态

一个组件,两个前端。插件是含 `cefnest-plugin.json` 清单的目录;
`"cefnest": {"type": "native"}` 把 `.so` 加载进宿主进程,`"type": "web"`
在页面上跑内容脚本与 `chrome.*` 子集。清单**形状**兼容
`manifest_version: 3`——字段名与含义是 Chrome 的——但这不是 Chrome 扩展
运行时:没有 service worker、没有 isolated world、没有扩展进程、没有商店。
兼容的含义是:按 MV3 形状写的包在这里能正确描述自己,而不是商店扩展能跑。

清单:

```json
{
  "manifest_version": 3,
  "name": "Example",
  "version": "1.0",
  "description": "…",
  "permissions": ["storage", "contextMenus"],
  "content_scripts": [{ "matches": ["https://example.com/*"],
                        "js": ["cs.js"], "run_at": "document_idle" }],
  "action": { "default_title": "Example", "default_popup": "popup.html" },
  "icons": { "16": "i16.png" },
  "cefnest": { "type": "native", "library": "plugin.so",
               "popup": { "width": 320, "height": 480 } }
}
```

- `id` = 目录名,`[a-z0-9_-]{1,64}`——它是插件每个
  `cefnest-extension://` URI 的 host 段。
- 组件永远不做的字段(`chrome_url_overrides`、`omnibox`、`commands`、
  `devtools_page`、`user_scripts`)**不许静默**:插件仍可用,同时
  `::plugin-error` 以 `unsupported-field:<name>` 逐个点名,这就是行副标题里
  "扩展为什么什么都没干"的答案。`background.service_worker` 不在此列:
  它会被真正执行(见下)。
- 被拒的清单(缺 `name`/`version`、类型不对、`cefnest` 块不可用)**留在
  模型里**,`state=refused` + 机器可读的 `error-message` token——拒绝的存在
  就是为了杜绝"目录在那儿、列表却什么都不说"。token:`no-manifest`、
  `bad-id`、`bad-manifest`、`unsupported-type`、`duplicate-id`。

## 插件住在哪里

**安装根**锚在 session 的[数据目录](data-directory.zh-CN.md)上,不锚在
组件名上:`<数据目录>/cefnest-plugins/`,每个插件一个子目录。它扫描在所有
`add_search_path()` 目录之前(用户自己的安装赢同 id 冲突);目录不存在是
空列表不是错误;**不设数据目录就没有任何默认插件位置**——"不指定目录就不
向磁盘写任何东西"的契约对插件同样成立。其他目录由宿主用
`add_search_path()` 给出;包用 `install_async()` 安装(`.zip` 或 `.crx`
——跳过 CRX3 外壳,外壳背后没有可校验的签名,这正是"只安装你自己构建的
插件"的原因)。

`chrome.storage` 的数据落在
`<数据目录>/cefnest-plugins/<id>/storage.json`——在 `clear_data()` 删的
那棵树里,持久化契约零额外成本。写盘按 500ms 定时合并;没有数据目录时
storage 只在内存,随进程消失。

## 插件能做什么

| 领域 | 原生(`.so`) | Web(内容脚本 + `chrome.*`) |
| --- | --- | --- |
| 右键菜单 | `add_menu_item(label, slot, cb, user_data)`——slot 0–199,菜单 id 26600+slot | `chrome.contextMenus.create({title, onclick})`——slot 由宿主分配 |
| 会话信号 | `subscribe("load-end", cb, …)`——按名字订阅任意 session 信号。回调携带信号自己的参数（`load-end` 是 `(session, browser, url, http_code, user_data)`），按旧签名写下的处理器必须跟着当前签名改 | — |
| Action | `set_action(widget)`——宿主决定摆在哪 | `chrome.action.onClicked.addListener` |
| 存储 | `storage_get` / `storage_set`(每 key 一段 JSON) | `chrome.storage.{local,session}`——`sync` 按本地实现并警告一次 |
| 页面 | — | `chrome.runtime.{id, sendMessage, getURL, onMessage}`、`chrome.tabs.{query, update}`(id/url/title/active/windowed/muted/zoom) |
| 内容脚本 | 清单的 `content_scripts` 同样生效 | 同左 |

两个前端可以混在一个包里:原生插件也可以带 `content_scripts`。

**内容脚本的时机。** `document_start` 是真的:脚本在页面自己的内联脚本
之前、在页面的 context 里运行(实测:页面第一个脚本看得见脚本留下的
标记)。机制值得知道,便于调试:插件表在渲染进程 spawn 时随命令行到达每个
渲染进程;脚本的字节经 `cefnest-extension://<id>/<path>` **同步**取回——
就是伺服插件页面的那个 origin——然后求值。`document_end` 与
`document_idle` 都在 load end 由浏览器进程侧执行。

**API 子集与它的接缝**,写下来是为了让插件作者分得清哪些是契约、哪些是
今天的形状:

- `chrome.runtime.sendMessage` 到达宿主与该插件自己打开着的扩展页面——
  常驻后台页也算;今天的应答是宿主对消息的回显。
- `chrome.runtime.onMessage` 的监听器会收到该插件各页面发出的每条消息,
  形状是解析后的值(宿主投递的是一条 JSON 文本;shim 在分发前解析)。多个
  插件同页时共享页级投递钩子——先装的 shim 持有它们,并跨每个 shim 的
  监听器风扇分发。
- `chrome.tabs.query` 由 helper 的浏览器表回答——id、url、title、active,
  外加从各 browser 的 host 现读的 `windowed`、`muted`、`zoom`;`update`
  导航、激活、静音。`pinned` 与 `discarded` 不存在:没有数据来源,造数
  即谎报。
- 子 frame 里的 shim 会永远等不到应答——应答只投递到主 frame。

**后台页。** 清单的 `background.service_worker` 指名一个脚本;真正跑起来
的是一个常驻**隐藏 browser**,加载
`cefnest-extension://<id>/__cefnest_background__.html`——一个合成页,
唯一职责是加载那个脚本。该 browser 没有窗口、没有 surface:嵌套合成器上
不出现任何东西,不占 toplevel,永不成为激活 browser,也不出现在
`tabs.query` 里。它是 Chromium 意义上的隐藏(`WasHidden`),渲染退到隐藏
标签页的节奏,而定时器、网络与存储照常运转。它的 `chrome.*` 与任何扩展
页面拿到的是同一个 shim,所以 `onMessage` 与 `chrome.storage` 在那里可用;
插件内容脚本的消息经 `chrome.runtime.sendMessage` 抵达它。

这**不是** Chrome 的 service worker。没有 SW 生命周期——没有事件唤醒、
没有挂起、没有 install/activate 阶段。页面存在的时长恰是插件启用的时长:
启用即创建,禁用即关闭,helper 重启即重建。Chrome 里经 SW 分发的事件
(alarms、webRequest、tabs 触发)这里不存在。清单字段转正,是因为那个
**形状**——插件作者写的一段脚本,启用期间常驻运行——如今是诚实的;语义
仍是 Chrome 自己的,这条边界写在本页。

**载荷上限:1 MiB。** shim 超了就拒发,浏览器进程收到再查,应答路径发送
再拒——页面上和宿主侧都叫 `plugin-payload-too-large`。插件的超大消息
绝不允许把控制通道打死:通道保持存活,紧随其后的小消息照常工作
(门禁实测)。

## 在这里永远做不了什么

- **内容脚本没有 isolated world。** CEF 没有这个原语(对着 CEF 树查过,
  不是猜的)。内容脚本与页面共用同一个 JS 全局:页面可以读它、覆盖它、
  调它挂上去的桥。因此内容脚本不持有特权——所有特权调用都转回宿主、按
  权限表裁决。即便如此,页面仍可以拿着插件的 id 直接调同一个桥来**冒充**
  插件;这条边界是架构本身的,本文档不假装它能修。两个插件同页时各装各的
  shim,最后写的 `window.chrome` 赢。
- **原生插件拥有宿主进程。** 没有沙箱、没有能力过滤:`.so` 拥有宿主的
  全部权限。默认策略:只扫数据目录的安装根;其他位置必须宿主显式指向。
  只安装你自己构建的插件。
- **没有 MV3 service worker 的*语义*。** `background.service_worker` 以常驻
  隐藏页运行(见上)——没有 SW 生命周期,没有事件唤醒。清单里任何其他
  background 形状(`page`、`scripts`)仍以 `unsupported-field:background`
  点名。
- **没有签名体系、没有商店、没有自动更新。** `install_async` 跳过 CRX3
  外壳,因为外壳背后没有可验的东西。

## 权限

词表沿用 WebExtension(`storage`、`tabs`、`contextMenus`、`activeTab`……);
一张表服务两个前端。`enable()` 遇到"声明了但表里还没有"时先发
`::grant-request` 再读表:宿主在发射返回前 grant,同一次调用直接成功;
宿主弹对话框,则这次拿到 `FALSE`、之后重新 enable。**没人应答的请求等于
拒绝**——这是这张表存在的理由。内建 handler 自己执行这张表:从没声明
`storage` 的插件调 `chrome.storage.local.get` 会被点名拒绝
(`::plugin-error` 上是 `permission-denied:storage`,页面拿到错误),其他
插件不受影响。

## 卸载

`disable()` 按宿主自己的记账反向回收:菜单行从 helper 的表上摘掉、信号
连接断开、widget 引用释放、`.so` unmap——插件只注册回调、不注册 GType,
它做过的一切都无法活得比卸载久。`::closed`(helper 死了)在任何信号可能
触达已 unmap 的 handler 之前跑同一套卸载。重启会重载仍启用的插件并补发
菜单行;门禁跑 disable → stop → start → enable → stop → start,并在
`G_DEBUG=fatal-criticals` 下数清每一次加载与卸载。
