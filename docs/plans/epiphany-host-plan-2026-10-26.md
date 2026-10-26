# epiphany 宿主缺口计划（2026-10-26）

本文立项**服务 epiphany（GTK 壳子 + cefnest 引擎）所需的组件层补充**。承接
[work-plan-2026-10-26.md](work-plan-2026-10-26.md)（已历史化）与
[response-filter-plan-2026-10-26.md](response-filter-plan-2026-10-26.md)
（独立推进，互不阻塞）。分类判据、四交互形态、工程约束全部沿用总计划
第一/四部分，本文只写缺口与增量，不重复裁定。

## 第零部分 · 现状锚点（2026-10-26 已核实）

总计划第 1–6 步已落地。已具备、本文**不再规划**的：

| 面 | 锚点 |
| --- | --- |
| 组件集成 | `proxy`/`proxy-bypass`/`accept-language`/`auto-resize`/`zoom-set` |
| 能力接口 | cookies get/set/delete；`print-pdf`（流式+settings）；`page-text`/`history`（只读）；`favicon`（URL 列表）；loading-progress/fullscreen/mute 上报 |
| 混合 | JS 对话框（alert/confirm/prompt/beforeunload）；文件对话框 |
| 策略 | `certificate-error` + respond + `certificate-authorize`（指纹豁免）；`request-intercept`（patterns、allow/cancel/redirect、passive）；CDP 管道（per-browser enable、execute、event 直通） |
| 插件 P3 | `browser-surface` 配对；background 页；browsers 表扩面（`id/url/title/active/windowed/muted/zoom`）；插件权限 parked |

CEF 156 头文件核实（`third_party/cef/dist/include/`）：

- `GetAuthCredentials`（cef_request_handler.h:165）、`OnSelectClientCertificate`
  （:210）、`CefRegisterSchemeHandlerFactory`（cef_scheme.h:65）、
  `OnLoadStart/OnLoadEnd/OnLoadingStateChange`（cef_load_handler.h）——**面在，未接**。
- **`GoToIndex`/`GoToOffset` 全 include 目录零命中**——CEF 公开 API 只能读
  导航历史（`GetNavigationEntries`），不能跳。历史跳转唯一公开路径是 CDP
  `Page.getNavigationHistory` + `Page.navigateToHistoryEntry`（B.2 据此立项）。
- 拼写检查只有右键菜单行（cef_context_menu_handler.h），无运行时开关面。

## 第一部分 · 设计原则（增量两条）

1. **组件为主：宿主看得见属性与方法，看不见 CDP**。UA 覆写、截图、
   分类型清数据、TLS 信息、历史跳转这类 CDP 已覆盖的，一律升为组件级
   属性/方法，CDP 只是实现细节——组件内部用 CDP 不要求宿主
   `cdp-enable`，那个开关只管"宿主面协议管道"（与 B.2 的裁定同一逻辑）。
   配方文档降为实现备注，不作为宿主用法。**策略接口只留给铁律项**：
   凭据与放行（C.1/C.2），其余一律按组件集成或能力接口收。
2. **优先级按 epiphany 壳子排序**：A（多标签）、B（生命周期/历史）、
   C（信任面）是壳子硬需求；D（页面服务）、E（弹窗特性）服务完整浏览器
   形态；F 杂项随手。

### 分级总表（组件为主裁定）

| 条目 | 级别 | 宿主动作 |
| --- | --- | --- |
| A.1 browser 字段补齐 | 组件集成（寻址补全） | 方法传目标 id |
| A.2 browsers 表上 session | 组件集成（数据上报） | 读 list、听信号 |
| A.3 popup 透传 | 组件集成（数据上报） | 听信号 |
| B.1 load-started + 双报修复 | 组件集成（行为修正） | 听信号 |
| B.2 历史跳转 | 组件集成（一次调用，组件全管道） | `history_go(index)` |
| B.3 会话恢复 | 配方（宿主侧实现，组件零改动） | — |
| C.1 HTTP 认证 | **策略接口**（凭据，铁律归宿主；默认 cancel） | 每次应答 |
| C.2 客户端证书 | **策略接口**（放行，铁律归宿主；默认不选） | 每次应答 |
| C.3 TLS 信息 | 能力接口（`get_tls_info`，CDP 内部实现） | 读一次 |
| D.1 scheme handler | 组件机制 + 宿主供数据（非策略） | 注册、喂数据 |
| D.2 宿主脚本注入 | 组件集成（注册一次，组件全管道） | add/remove |
| D.3 UA 属性 / 截图 / 分类型清数据 | **组件集成/能力接口**（CDP 为实现细节） | 设属性、调方法 |
| D.4 运行时设置 | CEF 无面，降级记录 | — |
| E 弹窗窗口特性 | 组件集成（参数 + 数据上报） | 传参、听信号 |
| F.1 版本握手 | 组件集成 | 读 getter |

整份计划 16 项：组件集成/能力接口 12 项，宿主配方 2 项（B.3/C.3 降级
路径），**策略接口仅 C.1/C.2 两项**——凭据与放行语义上不可能组件代决。

功能 → 条目速查：后台标签标题/地址陈旧→A.2；非活动标签导航→A.1；
"在新标签打开"区分前后台→A.3；历史菜单/会话恢复→B.2/B.3；历史服务
喂食→B.1；地址栏安全指示→C.3；HTTP 认证→C.1；客户端证书→C.2；
about/阅读模式/资源页→D.1+D.2；UA 切换器→D.3；分类型清除数据→D.3；
window.open 几何→E；宿主/helper 错配防护→F.1。

## 第二部分 · A 批 · 多标签补齐（壳子硬需求）

### A.1 剩余命令的 `browser` 字段

- **现状**：只有 `select-browser`/`close-browser`/`load`/`js`/`mute` 与
  `cdp-*` 认 `browser`，其余命令作用于活动浏览器。epiphany 里每个
  `EphyWebView` 对应一个浏览器，后台标签的后退/刷新/查找/缩放全被这条挡住。
- **形状**：`back`/`forward`/`reload`/`stop`、`zoom-in/out/reset/set`、
  `find`/`find-stop`、`print-pdf`、`page-text`、`history`、`view-source`
  补可选 `browser` 字段（缺省=活动，惯例不变；helper 端
  `FindSlotForBrowser` + 活动回退已有）。Session 方法签名直接加参
  （0.0.1 无 ABI 负担）。
- **门禁**：`event-shape-gate` 加"非活动浏览器独立导航"用例——两个
  browser 各自 load/back/forward，事件与读数互不串扰。

### A.2 browsers 表上 session

- **现状**：helper 有 `browsers-list`/`browsers` 事件（喂 chrome.tabs.query，
  槽位表含 `id/url/title/active/windowed/muted/zoom`），session 不暴露——
  epiphany 后台标签的标题/地址只能陈旧着。
- **形状**：`chromium_session_browsers_list()` 返回浏览器表（只读结构体
  数组或 GListModel，立项定），`::browsers-changed` 在表变化（创建/关闭/
  选中/标题地址变更）时发。地址/标题变更是否重推表：helper 端 Display
  handler 已把 title 写进槽位表，url 同源补齐后每变更带一次（低频，可接受；
  过频再降为宿主轮询）。Session 侧按 id 缓存，宿主读后台标签状态不再依赖
  select。
- **门禁**：`browsers-gate`——创建/关闭/选中/改标题四触发各断言一次
  `::browsers-changed`；非活动浏览器改地址后 list 读到新值。

### A.3 popup 透传

- **现状**：协议 `popup-requested` 已带 `disposition`/`gesture`/`browser`，
  session 信号只透传 URL——epiphany 区分中键后台标签与 target=_blank 前台
  标签需要前两者。
- **形状**：`::popup-requested (url, disposition, gesture, browser)`，
  纯 session 侧补发射参数。
- **门禁**：协议级两例（中键、`target=_blank`）断言字段到位。

## 第三部分 · B 批 · 生命周期与导航历史

### B.1 加载生命周期

- **现状**：`load-end`（失败也发、code 0）/`load-error`（失败双报：
  真错误 + ERR_ABORTED 跟随）/loading-progress/address/title。
- **CEF 面**：`OnLoadStart (browser, frame, transition_type)` +
  `frame->IsMain()`（已核实）。
- **形状**：`load-started {browser, url, transition, is_main_frame}`
  fire-and-forget。**修双报**：主框架失败收敛为恰一次 `load-error` +
  恰一次 `load-end (code)`，文档写明语义变化。epiphany 的历史服务、
  `EphyWebView` 状态机、地址栏刷新全吃这批。
- **门禁**：`history-gate` 加"每次导航 started/end 恰一次；失败页
  error 恰一次"断言（成功页/404 页/断网页三例）。

### B.2 历史跳转

- **CEF 面**：公开 API **无**跳转接口（见锚点）。路径是 CDP
  `Page.getNavigationHistory`（取 entries+currentIndex+entryId）+
  `Page.navigateToHistoryEntry (entryId)`。
- **形状（裁定）**：加动词 `history-go {browser, index}`，helper 端**内部
  走自己的 DevTools 客户端实现**，不要求宿主 `cdp-enable`——`cdp-enable`
  的语义是"把协议管道交给宿主"（安全开关），history-go 是个scoped 能力，
  两者不混。实现上复用 4.3 的观察者机器，临时挂 agent、跳完即拆，立项
  时实测 attach/detach 的代价（一次跳转毫秒级预期）。
- **Session API**：`chromium_session_history_go (self, browser, index)`，
  完成以既有 `address`/`load-*` 事件呈现（无独立回执）。
- **门禁**：`history-gate` 扩——三页导航后 `history-go(0)` 断言回到
  第一页、canGoBack/Forward 翻转。
- **降级记录**：若 helper 内部挂 agent 的实测代价不可接受，则 B.2 改为
  配方（宿主自付 `cdp-enable` 代价），文档写明。

### B.3 会话恢复配方（无代码）

壳子的会话恢复 = 宿主侧存 URL 列表 + 每标签 `history()` 快照；B.2 落地后
可恢复完整 back 栈（逐项 go 到 index）。写成 `docs/` 配方段 +
epiphany 侧实现，组件零改动。

## 第四部分 · C 批 · 信任面

### C.1 HTTP 认证

- **CEF 面**：`GetAuthCredentials`（RequestHandler，已核实：165 行）。
  现状未覆写——认证站直接失败。
- **形状**：**must-be-answered**（沿 certificate-error 先例，不答导航挂着）。
  `auth-required {browser, id, is_proxy, scheme, realm, origin}` →
  `auth-respond {id, user, password}` / `auth-cancel {id}` → 
  `callback->Continue(...)` / `Cancel()`；`auth-closed {id}` 弃单（浏览器
  关闭先例）。默认（无人接）= cancel，即请求以 401 失败——Chrome 语义。
- **Session API**：`::auth-required` + `chromium_session_auth_respond/cancel`。
- **门禁**：`auth-gate`——本地 python basic-auth 服务器，正密码/错密码/
  取消三路径，proxy 变体一例。

### C.2 客户端证书选择

- **CEF 面**：`OnSelectClientCertificate`（已核实：210 行）。
- **形状**：must-be-answered。`client-cert-request {browser, id, origin,
  certs[{subject, issuer, fingerprint}]}` → `client-cert-respond {id, index}`
  （-1=不选，默认）。证书来自系统 NSS 库，helper 不做选择策略。
- **门禁**：门禁环境起不了 mTLS——注入式断言（certificate-gate 的先例）。

### C.3 正常页面的证书信息（能力接口，CDP 内部实现）

- **现状**：错误路径有 `certificate-error` 的 subject/issuer/fingerprint；
  正常 https 页 CEF 公开面无 SSLStatus 取口。
- **形状**：`chromium_session_get_tls_info (self, browser, out…)）`——
  返回 security_state（secure/insecure/…）+ 证书链摘要（subject、issuer、
  fingerprint、有效期），组件内部走 CDP `Security` 域（enable + 
  `visibleSecurityStateChanged`）或 `Network.getCertificate`（156 CDP 是否
  开放，立项实测），宿主不接触 CDP、不碰 `cdp-enable`。地址栏"查看证书"
  对话框吃这一个方法。
- **降级**：实测不可得则不落接口，地址栏锁图标仅按 scheme +
  `certificate-error` 事件组合判定，"查看证书"入口隐藏——降级决定写进
  实测结论，不进 API。
- **门禁**：`tls-info-gate`——本地 https 探针页（自签）断言 insecure 态；
  豁免后（certificate-authorize）断言证书字段到位；注入式断言沿
  certificate-gate 先例。

## 第五部分 · D 批 · 页面服务（完整浏览器形态）

### D.1 宿主 scheme handler

- **CEF 面**：`CefRegisterSchemeHandlerFactory`（已核实）。现状宿主
  无法服务自定义 scheme——epiphany 的 `ephy-about:`/`ephy-reader:`/
  `ephy-resource:`/`ephy-view-source:` 全堵在这。
- **形状**：`scheme-register {name, flags{local, display_isolated, secure,
  cors}}` / `scheme-unregister {name}`。每请求 **parked + 流式**：
  `scheme-request {id, url, method, headers}` → 宿主
  `scheme-response {id, status, reason, content_type, headers}` →
  `scheme-chunk {id, data}`（base64 48 KiB → 64 KiB 行，先例沿用）→
  宿主 `scheme-end {id}`；中途 `scheme-abort {id}`；浏览器关闭弃单 +
  `scheme-closed {id}`。
- **安全**：scheme 名 `[a-z][a-z0-9+.-]*`；内建与 `cefnest-extension`
  不可覆盖；`secure`/`display_isolated` 标志直通 CEF 注册参数（epiphany
  的 about 页安全模型靠它）。
- **Session API**：`chromium_session_scheme_register/unregister` +
  `::scheme-request (id, url, method, headers)` + respond/write/abort。
- **门禁**：`scheme-gate`——注册 `app://` 服务一页断言渲染；
  `display_isolated` 下页面 fetch 跨 scheme 被拒；未注册 scheme 请求
  原样走网络（ERR_UNKNOWN_URL_SCHEME 不变）。

### D.2 宿主脚本注入（user-script）

- **现状**：页面 JS 注入只有插件 content scripts 一条路（渲染端插件表 +
  `cefnest-extension://`）；宿主没有等价物。`cefnestPostMessage` 的
  RenderProcessHandler 注入路径已在，机制可复用。
- **形状**：`script-add {id, browser?（缺省全浏览器）, patterns, code,
  run_at: document_start|document_end|document_idle, all_frames}` /
  `script-remove {id}`——与 `intercept-add` 同形（内存表、重加覆盖、
  无注册零开销）。CSS 不单独做 API（等价于脚本注入 `<style>`）；响应级
  CSS 等 filter 二期的 `inject-css`。
- **层界**：无隔离世界（总计划"明确不做"沿用）——宿主脚本与页面同 JS
  域运行，头文件注释写明。epiphany 用途：readability.js（阅读模式）、
  新标签页概览数据、（完整移植时的）表单钩子注入面。
- **门禁**：`script-gate`——`document_start` 先于页面内联脚本（插件
  content script 已实测的同一办法）；patterns 命中/不命中各一例；
  per-browser 与全局各一例。

### D.3 组件级集成：UA 属性、截图、分类型清数据（CDP 为实现细节）

三件都按"宿主设一次值/调一次方法，组件全包管道"收，与 proxy/
accept-language 的属性先例同形；CDP 调用全部发生在组件内部，宿主不需要
`cdp-enable`。

- **UA 覆写**：属性 `user-agent`（string，per-browser 可指定，缺省
  helper 启动值）——组件内部 `Emulation.setUserAgentOverride`。epiphany
  的 UA 切换器从"重启换 env"变成实时属性。
- **截图**：`chromium_session_capture_screenshot (self, browser) → GBytes`
  （PNG，receipt）——组件内部 `Page.captureScreenshot`。epiphany 的
  snapshot-service 缩略图吃这个，不再依赖合成器纹理回读。
- **分类型清数据**：`chromium_session_clear_data (self, types, error)`，
  types 位掩码：cache / cookies / auth-cache / origin-storage——组件内部
  分别走 `Network.clearBrowserCache` / `Network.clearCookies` /
  `Network.clearHttpAuthCache` / `Storage.clearDataForOrigin`（最后者
  带 origin 列表参数）。与既有 `clear_data()`（整目录删）并存：分类型走
  协议、全删走目录。epiphany 的"清除数据"首选项分类型语义由此恢复。
- **门禁**：`ua-gate`（探针页读 `navigator.userAgent` 前后比对）、
  `screenshot-gate`（PNG 魔数 + 尺寸 >0 + 与页面标记字符串的像素抽查）、
  `clear-data-gate`（cookie/缓存先置后清，读回断言空）。

### D.4 运行时设置（降级记录，无代码）

- 拼写检查：CEF 156 无运行时开关面，维持 spawn 环境变量，文档记"重启生效"。
- 自动播放：spawn 命令行（`--autoplay-policy`）一刀切，per-site 不可行——
  epiphany 的 per-site 自动播放权限降级为全局设置，文档记。
- JS 全局开关：WebPreferences 定型于浏览器创建，运行时不可切——映射为
  "重启生效"。

## 第六部分 · E 批 · 弹窗窗口特性

- **现状**：`new-browser(url, windowed)` 无几何；epiphany 的 window.open
  需要 features（尺寸/位置/工具栏可见性）。
- **形状**：`new-browser` 加可选 `geometry {x, y, w, h}`；新增
  fire-and-forget `window-properties {browser, x, y, w, h, toolbar,
  menubar, status, resizable, fullscreen}`（CEF 窗口创建/变更时上报，
  helper 从 CefWindow 现取）。
- **门禁**：`window-gate`——`window.open('', 'width=500,height=400')`
  断言尺寸传递；`menubar=no` 断言标志位。

## 第七部分 · F 批 · 杂项（小，随手）

1. **版本握手**：`ready` 事件加 `version` 字段 + 
   `chromium_session_get_helper_version()`。epiphany 经编译期路径/env 找
   helper，新旧错配的静默失败面真实存在；不匹配时 session `start()` 报错
   （大版本不一致）。
2. `close-browser` 关最后一个浏览器的语义文档化（现契约已写，核对
   epiphany 关窗路径即可，无代码）。

## 第八部分 · 实施顺序（每批独立可发布，门禁进 ci-all）

| 步 | 内容 | 完成标志 |
| --- | --- | --- |
| 1 | A.1 browser 字段补齐 + A.3 popup 透传 | event-shape 扩例绿 |
| 2 | C.1 HTTP 认证 + C.2 客户端证书 + C.3 TLS 信息 | auth-gate 绿、注入断言、tls-info-gate 绿 |
| 3 | B.1 生命周期 + 双报修复 | history-gate 扩例绿 |
| 4 | A.2 browsers 表上 session | browsers-gate 绿 |
| 5 | E 弹窗特性 + F.1 版本握手 | window-gate 绿 |
| 6 | D.1 scheme handler | scheme-gate 绿 |
| 7 | D.2 宿主脚本注入 | script-gate 绿 |
| 8 | B.2 历史跳转（先实测 helper 内部挂 agent 代价）+ D.3 组件级集成（UA 属性/截图/分类型清数据）+ B.3 配方文档 | history-go 实测数据 + ua/screenshot/clear-data 门禁绿 |

每步落地 `ci-all` 全量回归。第 1–4 步是 epiphany 壳子（P1/P2 阶段）的
依赖，5–8 随壳子功能排期。

## 第九部分 · 明确不做（本文范围）

隔离世界（CEF 无原语，总计划沿用）；表单捕获/密码管理/自动填充钩子
（完整移植的工程，壳子范围明确砍除，D.2 留了注入面但不立项）；ITP 摘要；
per-site 自动播放；拼写检查运行时开关（CEF 无公开面）；WebDriver
（CDP 覆盖自动化测试诉求）；打印对话框（宿主侧 GTK 事务）；
`chrome.*` 任何扩面（插件计划裁定不变）。

## 第十部分 · 落地记录（2026-10-26）

16 项全部落地，门禁进 `ci-all`。实现中与计划有出入、或计划留待实测的点，
结论记在这里（计划正文不改，实测结论逐条对应）：

| 条目 | 结论 |
| --- | --- |
| A.1 | 全部补齐；`back`/`forward`/`reload`/`stop`/`zoom-*`/`find*`/`view-source`/`page-text`/`history`/`print-pdf` 均带可选 `browser`。门禁：`multi-browser-gate.sh` 的"后台标签独立导航"用例 |
| A.2 | `::browsers-changed` 在创建/关闭/选中/标题/地址/静音/缩放时整表重推；`chromium_session_browsers_list()` 读缓存。门禁：`browsers-gate.sh` |
| A.3 | `::popup-requested (url, disposition, gesture, browser)`。**实测补充**：中键/ctrl+点击不走 `OnBeforePopup` 而走 `OnOpenURLFromTab`（后台标签 CEF 不建 popup），两条路径都接、都发同一事件；`OnOpenURLFromTab` 返回 true 并把非 `CEF_WOD_NEW_BACKGROUND_TAB` 的请求就地导航，原实现让中键直接顶掉当前页 |
| B.1 | `load-started` 落地；双报**收敛规则改为实测形态**：Chromium 在真实错误后约 1 秒补一条 `ERR_ABORTED`，而两者之间错误页自身会走一遍 start/end，所以"每次导航计数"会被重置——最终规则是"同 URL、`ERR_ABORTED`、5 秒内 ⇒ 回声"。门禁：`history-gate.sh` 三例（成功/404/拒连） |
| B.2 | `history-go` 走组件自有 DevTools 管道，**不要求 `cdp-enable`**。实测：廉价 CDP 调用会在 `ExecuteDevToolsMethod` 内**同步重入**回调，pending 表必须"先登记后调用"且消息 id 自己分配，否则结果必丢；观察者注册在回调内释放会自注销，改为延后一拍销毁。代价毫秒级，降级条款不触发 |
| B.3 | `docs/session-restore.md` / `.zh-CN.md` |
| C.1 | `GetAuthCredentials` 接上。**实测补充**：CEF 156（Chromium 128+）默认把认证交给自己的登录提示 UI，而 Alloy 无此 UI，回调根本不触发——必须加 `--disable-chrome-login-prompt`（上游 chromiumembedded/cef#3792）。门禁：`auth-gate.sh`（正/误/取消/407 代理四例，代理需 `<-loopback>` 才吃到 loopback 目标） |
| C.2 | `OnSelectClientCertificate` 接上，`client-cert-respond(index)`，-1=不选。门禁为注入式断言（`auth-gate.sh` 的 wiring 段）——本环境起不了 mTLS |
| C.3 | `get_tls_info` 落地；**实测**：CEF 156 后端没有 `Security.getVisibleSecurityState`（-32601），改走 `Security.enable` + `visibleSecurityStateChanged` 事件（内部事件路由、5 秒超时、用完 `Security.disable`）。自签页面的状态字是 `insecure-broken` 而非 `insecure`。门禁：`tls-info-gate.sh` |
| D.1 | scheme 的**选项**只能在进程启动时经 `CHROMIUM_HELPER_SCHEMES` 交给 `OnRegisterCustomSchemes`（与 proxy 同型），运行时 `scheme-register` 只挂数据工厂；计划原文"flags 直通 CEF 注册参数"的实时性做不到，已在协议文档写明。流式：parked Open → response → chunk* → end，48 KiB/片。门禁：`scheme-gate.sh` 三例 |
| D.2 | `script-add` / `script-remove`，document_start 走渲染进程（复用插件表的进程消息 + 命令行双通道），end/idle 走 OnLoadEnd；per-browser 与全局两级。无隔离世界照计划。门禁：`script-gate.sh` 三例 |
| D.3 | `user-agent` 属性 + `browser_set_user_agent`、`capture_screenshot`、`clear_data_types`，全部走组件自有管道。UA 的"回到默认"用 `Browser.getVersion` 捕获的原始值覆写回去（CDP 无 unset）。门禁：`ua-gate.sh`、`screenshot-gate.sh`（PNG 魔数+尺寸）、`clear-data-gate.sh` |
| D.4 | 无代码，降级记录照旧（拼写检查/自动播放/JS 开关均为重启生效） |
| E | `new_browser_with_geometry` + `::window-properties`；实现时窗口几何经 `GetInitialBounds` 生效，`OnWindowBoundsChanged`/全屏切换各重报一次。门禁：`window-gate.sh` |
| F.1 | `ready` 带 `version`（当前 2），session 大版本不符即停；`get_helper_version()`。**副作用修复**：`plugin-menu-command` 在无 manager 的 session 上会 critical，已加判空（event-shape 门禁原就暴露它） |

协议版本号在 helper 的 `kHelperProtocolVersion` 与 session 的
`SESSION_CHANNEL_MAJOR` 两处，改形状时同步。
